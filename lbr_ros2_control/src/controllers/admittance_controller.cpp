#include "lbr_ros2_control/controllers/admittance_controller.hpp"

namespace lbr_ros2_control {
AdmittanceController::AdmittanceController() {}

controller_interface::InterfaceConfiguration
AdmittanceController::command_interface_configuration() const {
  controller_interface::InterfaceConfiguration interface_configuration;
  interface_configuration.type = controller_interface::interface_configuration_type::INDIVIDUAL;
  for (const auto &joint_name : joint_names_) {
    interface_configuration.names.push_back(joint_name + "/" + hardware_interface::HW_IF_POSITION);
  }
  return interface_configuration;
}

controller_interface::InterfaceConfiguration
AdmittanceController::state_interface_configuration() const {
  controller_interface::InterfaceConfiguration interface_configuration;
  interface_configuration.type = controller_interface::interface_configuration_type::INDIVIDUAL;

  // joint position interface
  for (const auto &joint_name : joint_names_) {
    interface_configuration.names.push_back(joint_name + "/" + hardware_interface::HW_IF_POSITION);
  }

  // estimated force-torque sensor interface
  for (const auto &interface_name : estimated_ft_sensor_ptr_->get_state_interface_names()) {
    interface_configuration.names.push_back(interface_name);
  }

  // additional state interfaces
  interface_configuration.names.push_back(std::string(HW_IF_AUXILIARY_PREFIX) + "/" +
                                          HW_IF_SAMPLE_TIME);
  interface_configuration.names.push_back(std::string(HW_IF_AUXILIARY_PREFIX) + "/" +
                                          HW_IF_SESSION_STATE);
  return interface_configuration;
}

controller_interface::CallbackReturn AdmittanceController::on_init() {
  try {
    this->get_node()->declare_parameter("robot_name", "lbr");
    this->get_node()->declare_parameter("admittance.mass", 1.0);
    this->get_node()->declare_parameter("admittance.damping", 0.1);
    this->get_node()->declare_parameter("admittance.stiffness", 0.0);
    this->get_node()->declare_parameter("inv_jac_ctrl.chain_root", "lbr_link_0");
    this->get_node()->declare_parameter("inv_jac_ctrl.chain_tip", "lbr_link_ee");
    this->get_node()->declare_parameter("inv_jac_ctrl.damping", 0.2);
    this->get_node()->declare_parameter("inv_jac_ctrl.max_linear_velocity", 0.1);
    this->get_node()->declare_parameter("inv_jac_ctrl.max_angular_velocity", 0.1);
    configure_joint_names_();
    configure_admittance_impl_();
    configure_inv_jac_ctrl_impl_();
  } catch (const std::exception &e) {
    RCLCPP_ERROR(this->get_node()->get_logger(),
                 "Failed to initialize admittance controller with: %s.", e.what());
    return controller_interface::CallbackReturn::ERROR;
  }

  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::return_type AdmittanceController::update(const rclcpp::Time & /*time*/,
                                                               const rclcpp::Duration &period) {
  // get estimated force-torque sensor values
  f_ext_.head(3) =
      Eigen::Map<Eigen::Matrix<double, 3, 1>>(estimated_ft_sensor_ptr_->get_forces().data());
  f_ext_.tail(3) =
      Eigen::Map<Eigen::Matrix<double, 3, 1>>(estimated_ft_sensor_ptr_->get_torques().data());

  // get joint positions
  std::for_each(q_.begin(), q_.end(), [&, i = 0](double &q_i) mutable {
    q_i = this->state_interfaces_[i].get_value();
    ++i;
  });

  // compute forward kinematics
  auto chain_tip_frame = inv_jac_ctrl_impl_ptr_->get_kinematics_ptr()->compute_fk(q_);
  x_.head(3) = Eigen::Map<Eigen::Matrix<double, 3, 1>>(chain_tip_frame.p.data);
  chain_tip_frame.M.GetRPY(x_init_[3], x_init_[4], x_init_[5]);

  // compute steady state position and orientation
  if (!initialized_) {
    x_init_ = x_;
    x_prev_ = x_;
    initialized_ = true;
  }

  // compute velocity
  dx_ = (x_ - x_prev_) / period.seconds();

  // compute admittance
  admittance_impl_ptr_->compute(f_ext_, x_ - x_init_, dx_, ddx_);

  // update previous position
  x_prev_ = x_;

  // integrate ddx_ to command velocity
  twist_command_ = ddx_ * period.seconds();

  if (!inv_jac_ctrl_impl_ptr_) {
    RCLCPP_ERROR(this->get_node()->get_logger(), "Inverse Jacobian controller not initialized.");
    return controller_interface::return_type::ERROR;
  }
  if (static_cast<int>(session_state_interface_ptr_->get().get_value()) !=
      KUKA::FRI::ESessionState::COMMANDING_ACTIVE) {
    return controller_interface::return_type::OK;
  }

  // compute the joint velocity from the twist command target
  inv_jac_ctrl_impl_ptr_->compute(twist_command_, q_, dq_);

  // pass joint positions to hardware
  std::for_each(q_.begin(), q_.end(), [&, i = 0](const double &q_i) mutable {
    this->command_interfaces_[i].set_value(
        q_i + dq_[i] * sample_time_state_interface_ptr_->get().get_value());
    ++i;
  });

  return controller_interface::return_type::OK;
}

controller_interface::CallbackReturn
AdmittanceController::on_configure(const rclcpp_lifecycle::State & /*previous_state*/) {
  estimated_ft_sensor_ptr_ = std::make_unique<semantic_components::ForceTorqueSensor>(
      std::string(HW_IF_ESTIMATED_FT_PREFIX) + "/" + HW_IF_FORCE_X,
      std::string(HW_IF_ESTIMATED_FT_PREFIX) + "/" + HW_IF_FORCE_Y,
      std::string(HW_IF_ESTIMATED_FT_PREFIX) + "/" + HW_IF_FORCE_Z,
      std::string(HW_IF_ESTIMATED_FT_PREFIX) + "/" + HW_IF_TORQUE_X,
      std::string(HW_IF_ESTIMATED_FT_PREFIX) + "/" + HW_IF_TORQUE_Y,
      std::string(HW_IF_ESTIMATED_FT_PREFIX) + "/" + HW_IF_TORQUE_Z);
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn
AdmittanceController::on_activate(const rclcpp_lifecycle::State & /*previous_state*/) {
  if (!reference_state_interfaces_()) {
    return controller_interface::CallbackReturn::ERROR;
  }
  zero_all_values_();
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn
AdmittanceController::on_deactivate(const rclcpp_lifecycle::State & /*previous_state*/) {
  clear_state_interfaces_();
  return controller_interface::CallbackReturn::SUCCESS;
}

bool AdmittanceController::reference_state_interfaces_() {
  for (auto &state_interface : state_interfaces_) {
    if (state_interface.get_interface_name() == hardware_interface::HW_IF_POSITION) {
      joint_position_state_interfaces_.emplace_back(std::ref(state_interface));
    }
    if (state_interface.get_interface_name() == HW_IF_SAMPLE_TIME) {
      sample_time_state_interface_ptr_ =
          std::make_unique<std::reference_wrapper<hardware_interface::LoanedStateInterface>>(
              std::ref(state_interface));
    }
    if (state_interface.get_interface_name() == HW_IF_SESSION_STATE) {
      session_state_interface_ptr_ =
          std::make_unique<std::reference_wrapper<hardware_interface::LoanedStateInterface>>(
              std::ref(state_interface));
    }
  }
  if (!estimated_ft_sensor_ptr_->assign_loaned_state_interfaces(state_interfaces_)) {
    RCLCPP_ERROR(this->get_node()->get_logger(),
                 "Failed to assign estimated force torque state interfaces.");
    return false;
  }
  if (joint_position_state_interfaces_.size() != lbr_fri_ros2::N_JNTS) {
    RCLCPP_ERROR(
        this->get_node()->get_logger(),
        "Number of joint position state interfaces '%ld' does not match the number of joints "
        "in the robot '%d'.",
        joint_position_state_interfaces_.size(), lbr_fri_ros2::N_JNTS);
    return false;
  }
  return true;
}

void AdmittanceController::clear_state_interfaces_() {
  joint_position_state_interfaces_.clear();
  estimated_ft_sensor_ptr_->release_interfaces();
}

void AdmittanceController::configure_joint_names_() {
  if (joint_names_.size() != lbr_fri_ros2::N_JNTS) {
    RCLCPP_ERROR(
        this->get_node()->get_logger(),
        "Number of joint names (%ld) does not match the number of joints in the robot (%d).",
        joint_names_.size(), lbr_fri_ros2::N_JNTS);
    throw std::runtime_error("Failed to configure joint names.");
  }
  std::string robot_name = this->get_node()->get_parameter("robot_name").as_string();
  for (int i = 0; i < lbr_fri_ros2::N_JNTS; ++i) {
    joint_names_[i] = robot_name + "_A" + std::to_string(i + 1);
  }
}

void AdmittanceController::configure_admittance_impl_() {
  admittance_impl_ptr_ = std::make_unique<AdmittanceImpl>(
      AdmittanceParameters{this->get_node()->get_parameter("admittance.mass").as_double(),
                           this->get_node()->get_parameter("admittance.damping").as_double(),
                           this->get_node()->get_parameter("admittance.stiffness").as_double()});
  RCLCPP_INFO(this->get_node()->get_logger(), "Admittance controller initialized.");
  RCLCPP_INFO(this->get_node()->get_logger(), "Mass: %f",
              this->get_node()->get_parameter("admittance.mass").as_double());
  RCLCPP_INFO(this->get_node()->get_logger(), "Damping: %f",
              this->get_node()->get_parameter("admittance.damping").as_double());
  RCLCPP_INFO(this->get_node()->get_logger(), "Stiffness: %f",
              this->get_node()->get_parameter("admittance.stiffness").as_double());
}

void AdmittanceController::configure_inv_jac_ctrl_impl_() {
  inv_jac_ctrl_impl_ptr_ = std::make_unique<lbr_fri_ros2::InvJacCtrlImpl>(
      this->get_robot_description(),
      lbr_fri_ros2::InvJacCtrlParameters{
          this->get_node()->get_parameter("inv_jac_ctrl.chain_root").as_string(),
          this->get_node()->get_parameter("inv_jac_ctrl.chain_tip").as_string(),
          true, // always assume twist in tip frame, since force-torque is estimated in tip frame
          this->get_node()->get_parameter("inv_jac_ctrl.damping").as_double(),
          this->get_node()->get_parameter("inv_jac_ctrl.max_linear_velocity").as_double(),
          this->get_node()->get_parameter("inv_jac_ctrl.max_angular_velocity").as_double()});
}

void AdmittanceController::zero_all_values_() {
  f_ext_.setZero();
  x_.setZero();
  dx_.setZero();
  ddx_.setZero();
  std::fill(dq_.begin(), dq_.end(), 0.0);
  twist_command_.setZero();
}
} // namespace lbr_ros2_control

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(lbr_ros2_control::AdmittanceController,
                       controller_interface::ControllerInterface)