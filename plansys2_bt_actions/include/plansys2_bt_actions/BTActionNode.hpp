// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PLANSYS2_BT_ACTIONS__BTACTIONNODE_HPP_
#define PLANSYS2_BT_ACTIONS__BTACTIONNODE_HPP_

#include <memory>
#include <string>

#include "behaviortree_cpp_v3/action_node.h"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "rclcpp_action/rclcpp_action.hpp"

namespace plansys2
{

using namespace std::chrono_literals;  // NOLINT

template<class ActionT, class NodeT = rclcpp::Node>
class BtActionNode : public BT::ActionNodeBase
{
public:
  BtActionNode(
    const std::string & xml_tag_name,
    const std::string & action_name,
    const BT::NodeConfiguration & conf)
  : BT::ActionNodeBase(xml_tag_name, conf), action_name_(action_name),
    return_failure_(false)
  {
    node_ = config().blackboard->get<typename NodeT::SharedPtr>("node");


    // Initialize the input and output messages
    goal_ = typename ActionT::Goal();
    result_ = typename rclcpp_action::ClientGoalHandle<ActionT>::WrappedResult();

    std::string remapped_action_name;
    if (getInput("server_name", remapped_action_name)) {
      action_name_ = remapped_action_name;
    }

    // Give the derive class a chance to do any initialization
    RCLCPP_INFO(node_->get_logger(), "\"%s\" BtActionNode initialized", xml_tag_name.c_str());
  }

  BtActionNode() = delete;

  virtual ~BtActionNode()
  {
  }

  // Create instance of an action server
  bool createActionClient(const std::string & action_name)
  {
    // Now that we have the ROS node to use, create the action client for this BT action
    action_client_ = rclcpp_action::create_client<ActionT>(node_, action_name);

    // Make sure the server is actually there before continuing
    RCLCPP_INFO(node_->get_logger(), "Waiting for \"%s\" action server", action_name.c_str());
    return action_client_->wait_for_action_server(server_timeout_);
  }

  // Any subclass of BtActionNode that accepts parameters must provide a providedPorts method
  // and call providedBasicPorts in it.
  static BT::PortsList providedBasicPorts(BT::PortsList addition)
  {
    BT::PortsList basic = {
      BT::InputPort<std::string>("server_name", "Action server name"),
      BT::InputPort<unsigned int>(
        "server_timeout",
        1000,
        "The amount of time to wait for a response from the action server, "
        "in units of milliseconds")
    };
    basic.insert(addition.begin(), addition.end());

    return basic;
  }

  static BT::PortsList providedPorts()
  {
    return providedBasicPorts({});
  }

  // Derived classes can override any of the following methods to hook into the
  // processing for the action: on_tick, on_feedback, on_wait_for_result,
  // and on_success

  // Could do dynamic checks, such as getting updates to values on the blackboard
  virtual void on_tick()
  {
  }

  // Provides the opportunity for derived classes to log feedback, update the
  // goal, or cancel the goal
  virtual void on_feedback(
    const std::shared_ptr<const typename ActionT::Feedback> feedback)
  {
  }

  // There can be many loop iterations per tick. Any opportunity to do something after
  // a timeout waiting for a result that hasn't been received yet
  virtual void on_wait_for_result()
  {
  }

  // Called upon successful completion of the action. A derived class can override this
  // method to put a value on the blackboard, for example.
  virtual BT::NodeStatus on_success()
  {
    return BT::NodeStatus::SUCCESS;
  }

  // Called when a the action is aborted. By default, the node will return FAILURE.
  // The user may override it to return another value, instead.
  virtual BT::NodeStatus on_aborted()
  {
    return BT::NodeStatus::FAILURE;
  }

  // Called when a the action is cancelled. By default, the node will return SUCCESS.
  // The user may override it to return another value, instead.
  virtual BT::NodeStatus on_cancelled()
  {
    return BT::NodeStatus::SUCCESS;
  }

  // The main override required by a BT action
  BT::NodeStatus tick() override
  {
    // first step to be done only at the beginning of the Action
    if (status() == BT::NodeStatus::IDLE) {
      // Get the required items from the blackboard
      unsigned int server_timeout_int;
      if (!getInput<unsigned int>("server_timeout", server_timeout_int)) {
        // This will only happen if `providedPorts` is overridden and
        // the child class does not provide the "server_timeout" port.
        // Child classes can use the `providedBasicPorts` method to avoid
        // this issue
        RCLCPP_INFO(
          node_->get_logger(),
          "Missing input port [server_timeout], "
          "using default value of 1s");
        RCLCPP_DEBUG(
          node_->get_logger(),
          "Use the `providedBasicPorts` method to avoid this issue");
        server_timeout_int = 1000;
      }
      server_timeout_ = std::chrono::milliseconds(server_timeout_int);

      if (!createActionClient(action_name_)) {
        RCLCPP_ERROR(node_->get_logger(), "Could not create action client");
        return BT::NodeStatus::FAILURE;
      }

      // setting the status to RUNNING to notify the BT Loggers (if any)
      setStatus(BT::NodeStatus::RUNNING);

      // user defined callback
      on_tick();
      if (return_failure_) {
        return BT::NodeStatus::FAILURE;
      }

      on_new_goal_received();
      if (return_failure_) {
        cancel_goal();
        return BT::NodeStatus::FAILURE;
      }
    }

    // The following code corresponds to the "RUNNING" loop
    if (rclcpp::ok() && !goal_result_available_) {
      // user defined callback. May modify the value of "goal_updated_"
      on_wait_for_result();
      if (return_failure_) {
        cancel_goal();
        return BT::NodeStatus::FAILURE;
      }

      auto goal_status = goal_handle_->get_status();
      if (goal_updated_ && (goal_status == action_msgs::msg::GoalStatus::STATUS_EXECUTING ||
        goal_status == action_msgs::msg::GoalStatus::STATUS_ACCEPTED))
      {
        goal_updated_ = false;
        on_new_goal_received();
        if (return_failure_) {
          cancel_goal();
          return BT::NodeStatus::FAILURE;
        }
      }

      rclcpp::spin_some(node_->get_node_base_interface());

      // check if a derived class has set return_failure_ in a callback
      if (return_failure_) {
        cancel_goal();
        return BT::NodeStatus::FAILURE;
      }

      // check if, after invoking spin_some(), we finally received the result
      if (!goal_result_available_) {
        // Yield this Action, returning RUNNING
        return BT::NodeStatus::RUNNING;
      }
    }

    switch (result_.code) {
      case rclcpp_action::ResultCode::SUCCEEDED:
        return on_success();

      case rclcpp_action::ResultCode::ABORTED:
        return on_aborted();

      case rclcpp_action::ResultCode::CANCELED:
        return on_cancelled();

      default:
        RCLCPP_ERROR(
          node_->get_logger(),
          "BtActionNode::Tick: invalid rclcpp_action::ResultCode");
        return BT::NodeStatus::FAILURE;
    }
  }

  // The other (optional) override required by a BT action. In this case, we
  // make sure to cancel the ROS2 action if it is still running.
  void halt() override
  {
    cancel_goal();

    action_client_ = nullptr;
    setStatus(BT::NodeStatus::IDLE);
  }

protected:
  void cancel_goal()
  {
    // Shut the node down if it is currently running
    if (status() != BT::NodeStatus::RUNNING) {
      return;
    }

    rclcpp::spin_some(node_->get_node_base_interface());
    auto status = goal_handle_->get_status();

    // Check if the goal is still executing
    if (status == action_msgs::msg::GoalStatus::STATUS_ACCEPTED ||
      status == action_msgs::msg::GoalStatus::STATUS_EXECUTING)
    {
      auto future_cancel = action_client_->async_cancel_goal(goal_handle_);
      if (rclcpp::spin_until_future_complete(
          node_->get_node_base_interface(), future_cancel, server_timeout_) !=
        rclcpp::FutureReturnCode::SUCCESS)
      {
        RCLCPP_ERROR(
          node_->get_logger(),
          "Failed to cancel action server for %s", action_name_.c_str());
      } else {
        RCLCPP_INFO(
          node_->get_logger(),
          "Cancelled goal for action server %s",
          action_name_.c_str());
      }
    }
  }


  void on_new_goal_received()
  {
    goal_result_available_ = false;
    auto send_goal_options = typename rclcpp_action::Client<ActionT>::SendGoalOptions();
    send_goal_options.result_callback =
      [this](const typename rclcpp_action::ClientGoalHandle<ActionT>::WrappedResult & result) {
        // TODO(#1652): a work around until rcl_action interface is updated
        // if goal ids are not matched, the older goal call this callback so ignore the result
        // if matched, it must be processed (including aborted)
        if (this->goal_handle_->get_goal_id() == result.goal_id) {
          goal_result_available_ = true;
          result_ = result;
        }
      };
    send_goal_options.feedback_callback =
      [this](typename rclcpp_action::ClientGoalHandle<ActionT>::SharedPtr,
        const std::shared_ptr<const typename ActionT::Feedback> feedback) {
        on_feedback(feedback);
      };

    auto future_goal_handle = action_client_->async_send_goal(goal_, send_goal_options);

    if (rclcpp::spin_until_future_complete(
        node_->get_node_base_interface(), future_goal_handle, server_timeout_) !=
      rclcpp::FutureReturnCode::SUCCESS)
    {
      return_failure_ = true;
      RCLCPP_ERROR(
        node_->get_logger(),
        "Failed to send goal to action server %s",
        action_name_.c_str());
    }

    goal_handle_ = future_goal_handle.get();
    if (!goal_handle_) {
      return_failure_ = true;
      RCLCPP_ERROR(
        node_->get_logger(),
        "Goal was rejected by action server %s",
        action_name_.c_str());
    }
  }

  void increment_recovery_count()
  {
    int recovery_count = 0;
    config().blackboard->get<int>("number_recoveries", recovery_count);  // NOLINT
    recovery_count += 1;
    config().blackboard->set<int>("number_recoveries", recovery_count);  // NOLINT
  }

  std::string action_name_;
  typename std::shared_ptr<rclcpp_action::Client<ActionT>> action_client_;

  // All ROS2 actions have a goal and a result
  typename ActionT::Goal goal_;
  bool goal_updated_{false};
  bool goal_result_available_{false};
  typename rclcpp_action::ClientGoalHandle<ActionT>::SharedPtr goal_handle_;
  typename rclcpp_action::ClientGoalHandle<ActionT>::WrappedResult result_;

  // The node that will be used for any ROS operations
  typename NodeT::SharedPtr node_;

  // The timeout value while waiting for response from a server when a
  // new action goal is sent or canceled
  std::chrono::milliseconds server_timeout_;

  // A variable to signal a failure and return BT::NodeStatus::FAILURE
  bool return_failure_;
};


}  // namespace plansys2

#endif  // PLANSYS2_BT_ACTIONS__BTACTIONNODE_HPP_
