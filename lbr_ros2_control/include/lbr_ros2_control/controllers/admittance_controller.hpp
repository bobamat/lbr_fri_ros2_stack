#ifndef LBR_ROS2_CONTROL__ADMITTANCE_CONTROLLER_HPP_
#define LBR_ROS2_CONTROL__ADMITTANCE_CONTROLLER_HPP_

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "controller_interface/controller_interface.hpp"
#include "eigen3/Eigen/Core"
#include "hardware_interface/loaned_state_interface.hpp"
#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "rclcpp/rclcpp.hpp"
#include "semantic_components/force_torque_sensor.hpp"

#include "friLBRState.h"
#include "lbr_fri_ros2/control.hpp"
#include "lbr_fri_ros2/types.hpp"
#include "lbr_ros2_control/system_interface_type_values.hpp"

namespace lbr_ros2_control {
struct AdmittanceParameters {
  double m = 1.0;
  double b = 0.1;
  double k = 0.0;
};

class AdmittanceImpl {
public:
  AdmittanceImpl(const AdmittanceParameters &parameters) : parameters_(parameters) {}

  void compute(const Eigen::Matrix<double, lbr_fri_ros2::CARTESIAN_DOF, 1> &f_ext,
               const Eigen::Matrix<double, lbr_fri_ros2::CARTESIAN_DOF, 1> &x,
               const Eigen::Matrix<double, lbr_fri_ros2::CARTESIAN_DOF, 1> &dx,
               Eigen::Matrix<double, lbr_fri_ros2::CARTESIAN_DOF, 1> &ddx) {
    ddx = (f_ext - parameters_.b * dx - parameters_.k * x) / parameters_.m;
  }

protected:
  AdmittanceParameters parameters_;
};

class AdmittanceController : public controller_interface::ControllerInterface {
public:
  AdmittanceController();

  controller_interface::InterfaceConfiguration command_interface_configuration() const override;

  controller_interface::InterfaceConfiguration state_interface_configuration() const override;

  controller_interface::CallbackReturn on_init() override;

  controller_interface::return_type update(const rclcpp::Time &time,
                                           const rclcpp::Duration &period) override;

  controller_interface::CallbackReturn
  on_configure(const rclcpp_lifecycle::State &previous_state) override;

  controller_interface::CallbackReturn
  on_activate(const rclcpp_lifecycle::State &previous_state) override;

  controller_interface::CallbackReturn
  on_deactivate(const rclcpp_lifecycle::State &previous_state) override;

protected:
  bool reference_state_interfaces_();
  void clear_state_interfaces_();
  void configure_joint_names_();
  void configure_admittance_impl_();
  void configure_inv_jac_ctrl_impl_();
  void zero_all_values_();

  // admittance
  bool initialized_ = false;
  std::unique_ptr<AdmittanceImpl> admittance_impl_ptr_;
  Eigen::Matrix<double, lbr_fri_ros2::CARTESIAN_DOF, 1> x_init_, x_prev_;
  Eigen::Matrix<double, lbr_fri_ros2::CARTESIAN_DOF, 1> f_ext_, x_, dx_, ddx_;

  // joint veloctiy computation
  std::unique_ptr<lbr_fri_ros2::InvJacCtrlImpl> inv_jac_ctrl_impl_ptr_;
  lbr_fri_ros2::jnt_array_t q_, dq_;
  Eigen::Matrix<double, lbr_fri_ros2::CARTESIAN_DOF, 1> twist_command_;

  // interfaces
  lbr_fri_ros2::jnt_name_array_t joint_names_;
  std::vector<std::reference_wrapper<hardware_interface::LoanedStateInterface>>
      joint_position_state_interfaces_;
  std::unique_ptr<std::reference_wrapper<hardware_interface::LoanedStateInterface>>
      sample_time_state_interface_ptr_;
  std::unique_ptr<std::reference_wrapper<hardware_interface::LoanedStateInterface>>
      session_state_interface_ptr_;
  std::unique_ptr<semantic_components::ForceTorqueSensor> estimated_ft_sensor_ptr_;
};
} // namespace lbr_ros2_control
#endif // LBR_ROS2_CONTROL__ADMITTANCE_CONTROLLER_HPP_
