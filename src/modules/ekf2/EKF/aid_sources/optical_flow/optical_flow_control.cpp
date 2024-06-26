/****************************************************************************
 *
 *   Copyright (c) 2022 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file optical_flow_control.cpp
 * Control functions for optical flow fusion
 */

#include "ekf.h"

#include <ekf_derivation/generated/compute_flow_xy_innov_var_and_hx.h>

void Ekf::controlOpticalFlowFusion(const imuSample &imu_delayed)
{
	if (!_flow_buffer || (_params.flow_ctrl != 1)) {
		stopFlowFusion();
		return;
	}

	bool flow_data_ready = false;

	VectorState H;

	// New optical flow data is available and is ready to be fused when the midpoint of the sample falls behind the fusion time horizon
	if (_flow_buffer->pop_first_older_than(imu_delayed.time_us, &_flow_sample_delayed)) {
		flowSample &flow_sample = _flow_sample_delayed;

		const int32_t min_quality = _control_status.flags.in_air
					    ? _params.flow_qual_min
					    : _params.flow_qual_min_gnd;

		const bool is_quality_good = (flow_sample.quality >= min_quality);
		const bool is_magnitude_good = flow_sample.flow_rate.isAllFinite()
					       && !flow_sample.flow_rate.longerThan(_flow_max_rate);

		bool is_tilt_good = true;

#if defined(CONFIG_EKF2_RANGE_FINDER)
		is_tilt_good = (_R_to_earth(2, 2) > _params.range_cos_max_tilt);

#endif // CONFIG_EKF2_RANGE_FINDER

		if (is_quality_good
		    && is_magnitude_good
		    && is_tilt_good) {

			calcOptFlowBodyRateComp(imu_delayed, flow_sample);

			flow_data_ready = true;
		}

		// calculate optical LOS rates using optical flow rates that have had the body angular rate contribution removed
		// correct for gyro bias errors in the data used to do the motion compensation
		// Note the sign convention used: A positive LOS rate is a RH rotation of the scene about that axis.
		const Vector3f flow_gyro_corrected = flow_sample.gyro_rate - _flow_gyro_bias;
		const Vector2f flow_compensated = flow_sample.flow_rate - flow_gyro_corrected.xy();

		// calculate the optical flow observation variance
		const float R_LOS = calcOptFlowMeasVar(flow_sample);

		Vector2f innov_var;
		sym::ComputeFlowXyInnovVarAndHx(_state.vector(), P, R_LOS, FLT_EPSILON, &innov_var, &H);

		// run the innovation consistency check and record result
		updateAidSourceStatus(_aid_src_optical_flow,
				      flow_sample.time_us,                                 // sample timestamp
				      flow_compensated,                                    // observation
				      Vector2f{R_LOS, R_LOS},                              // observation variance
				      predictFlow(flow_gyro_corrected) - flow_compensated, // innovation
				      innov_var,                                           // innovation variance
				      math::max(_params.flow_innov_gate, 1.f));            // innovation gate

		// logging
		_flow_rate_compensated = flow_compensated;

		// compute the velocities in body and local frames from corrected optical flow measurement for logging only
		const float range = predictFlowRange();
		_flow_vel_body(0) = -flow_compensated(1) * range;
		_flow_vel_body(1) =  flow_compensated(0) * range;
		_flow_vel_ne = Vector2f(_R_to_earth * Vector3f(_flow_vel_body(0), _flow_vel_body(1), 0.f));
	}

	if (flow_data_ready) {

		// Check if we are in-air and require optical flow to control position drift
		bool is_flow_required = _control_status.flags.in_air
					&& (_control_status.flags.inertial_dead_reckoning // is doing inertial dead-reckoning so must constrain drift urgently
					    || isOnlyActiveSourceOfHorizontalAiding(_control_status.flags.opt_flow));

		const bool is_within_max_sensor_dist = getHagl() <= _flow_max_distance;

		const bool continuing_conditions_passing = (_params.flow_ctrl == 1)
				&& _control_status.flags.tilt_align
				&& is_within_max_sensor_dist;

		const bool starting_conditions_passing = continuing_conditions_passing
				&& isTimedOut(_aid_src_optical_flow.time_last_fuse, (uint64_t)2e6); // Prevent rapid switching

		// If the height is relative to the ground, terrain height cannot be observed.
		_hagl_sensor_status.flags.flow = _control_status.flags.opt_flow && !(_height_sensor_ref == HeightSensor::RANGE);

		if (_control_status.flags.opt_flow) {
			if (continuing_conditions_passing) {
				fuseOptFlow(H, _hagl_sensor_status.flags.flow);

				// handle the case when we have optical flow, are reliant on it, but have not been using it for an extended period
				if (isTimedOut(_aid_src_optical_flow.time_last_fuse, _params.no_aid_timeout_max)) {
					if (is_flow_required) {
						resetFlowFusion();

					} else {
						stopFlowFusion();
					}
				}

			} else {
				stopFlowFusion();
			}

		} else {
			if (starting_conditions_passing) {
				// If the height is relative to the ground, terrain height cannot be observed.
				_hagl_sensor_status.flags.flow = (_height_sensor_ref != HeightSensor::RANGE);

				if (isHorizontalAidingActive()) {
					if (fuseOptFlow(H, _hagl_sensor_status.flags.flow)) {
						ECL_INFO("starting optical flow");
						_control_status.flags.opt_flow = true;

					} else if (_hagl_sensor_status.flags.flow && !_hagl_sensor_status.flags.range_finder) {
						ECL_INFO("starting optical flow, resetting terrain");
						resetTerrainToFlow();
						_control_status.flags.opt_flow = true;
					}

				} else {
					if (isTerrainEstimateValid() || (_height_sensor_ref == HeightSensor::RANGE)) {
						ECL_INFO("starting optical flow, resetting");
						resetFlowFusion();
						_control_status.flags.opt_flow = true;

					} else if (_hagl_sensor_status.flags.flow) {
						ECL_INFO("starting optical flow, resetting terrain");
						resetTerrainToFlow();
						_control_status.flags.opt_flow = true;
					}
				}

				_hagl_sensor_status.flags.flow = _control_status.flags.opt_flow && !(_height_sensor_ref == HeightSensor::RANGE);
			}
		}

	} else if (_control_status.flags.opt_flow && isTimedOut(_flow_sample_delayed.time_us, _params.reset_timeout_max)) {
		stopFlowFusion();
	}
}

void Ekf::resetFlowFusion()
{
	ECL_INFO("reset velocity to flow");
	_information_events.flags.reset_vel_to_flow = true;
	resetHorizontalVelocityTo(_flow_vel_ne, calcOptFlowMeasVar(_flow_sample_delayed));

	// reset position, estimate is relative to initial position in this mode, so we start with zero error
	if (!_control_status.flags.in_air) {
		ECL_INFO("reset position to zero");
		resetHorizontalPositionTo(Vector2f(0.f, 0.f), 0.f);
	}

	resetAidSourceStatusZeroInnovation(_aid_src_optical_flow);

	_innov_check_fail_status.flags.reject_optflow_X = false;
	_innov_check_fail_status.flags.reject_optflow_Y = false;
}

void Ekf::resetTerrainToFlow()
{
	ECL_INFO("reset hagl to flow");
	// TODO: use the flow data
	_state.terrain = fmaxf(0.0f, _state.pos(2));
	P.uncorrelateCovarianceSetVariance<State::terrain.dof>(State::terrain.idx, 100.f);
	_terrain_vpos_reset_counter++;

	resetAidSourceStatusZeroInnovation(_aid_src_optical_flow);

	_innov_check_fail_status.flags.reject_optflow_X = false;
	_innov_check_fail_status.flags.reject_optflow_Y = false;
}

void Ekf::stopFlowFusion()
{
	if (_control_status.flags.opt_flow) {
		ECL_INFO("stopping optical flow fusion");
		_control_status.flags.opt_flow = false;
		_hagl_sensor_status.flags.flow = false;

		_fault_status.flags.bad_optflow_X = false;
		_fault_status.flags.bad_optflow_Y = false;

		_innov_check_fail_status.flags.reject_optflow_X = false;
		_innov_check_fail_status.flags.reject_optflow_Y = false;
	}
}

void Ekf::calcOptFlowBodyRateComp(const imuSample &imu, flowSample &flow_sample)
{
	// flow gyro has opposite sign convention
	const Vector3f ref_body_rate = -_angular_velocity_delayed;

	if (!PX4_ISFINITE(flow_sample.gyro_rate(0)) || !PX4_ISFINITE(flow_sample.gyro_rate(1))) {
		flow_sample.gyro_rate = ref_body_rate;
		_flow_gyro_bias.zero();

	} else if (!PX4_ISFINITE(flow_sample.gyro_rate(2))) {
		// Some flow modules only provide X ind Y angular rates. If this is the case, complete the vector with our own Z gyro
		flow_sample.gyro_rate(2) = ref_body_rate(2);
		_flow_gyro_bias(2) = 0.f;
	}

	// calculate the bias estimate using a combined LPF and spike filter
	_flow_gyro_bias = 0.99f * _flow_gyro_bias
			  + 0.01f * matrix::constrain(flow_sample.gyro_rate - ref_body_rate, -0.1f, 0.1f);
}
