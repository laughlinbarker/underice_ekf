/*
 * Copyright 2013 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
/*
 * Desc: 3D position interface for ground truth.
 * Author: Sachin Chitta and John Hsu
 * Date: 1 June 2008
 */

#include "../include/laughlin_uw_plugins/usbl_plugin.hpp"
#include <math.h>
#include <random>


namespace gazebo
{
// Register this plugin with the simulator
GZ_REGISTER_SENSOR_PLUGIN(USBLPlugin)

////////////////////////////////////////////////////////////////////////////////
// Constructor
USBLPlugin::USBLPlugin()
{
  this->seed = 0;
}

////////////////////////////////////////////////////////////////////////////////
// Destructor
USBLPlugin::~USBLPlugin()
{
  event::Events::DisconnectWorldUpdateBegin(this->update_connection_);
  // Finalize the controller
  this->rosnode_->shutdown();
  this->callback_queue_thread_.join();
  delete this->rosnode_;
}

////////////////////////////////////////////////////////////////////////////////
// Load the controller
void USBLPlugin::Load(sensors::SensorPtr _parent, sdf::ElementPtr _sdf)
{
  // save pointers
	# if GAZEBO_MAJOR_VERSION >= 7
	  std::string worldName = _parent->WorldName();
	# else
	  std::string worldName = _parent->GetWorldName();
	# endif

	  this->world_ = physics::get_world(worldName);
  this->sdf = _sdf;
  this->sensorParent = _parent;

  // ros callback queue for processing subscription
  this->deferred_load_thread_ = boost::thread(
    boost::bind(&USBLPlugin::LoadThread, this));
}

////////////////////////////////////////////////////////////////////////////////
// Load the controller
void USBLPlugin::LoadThread()
{
  // load parameters
  this->robot_namespace_ = "";
  if (this->sdf->HasElement("robotNamespace"))
    this->robot_namespace_ = this->sdf->Get<std::string>("robotNamespace");

  if (!this->sdf->HasElement("topicName"))
  {
    ROS_INFO("USBL plugin missing <topicName>, defaults to /usbl");
    this->topic_name_ = "/usbl";
  }
  else
    this->topic_name_ = "/" + this->sdf->Get<std::string>("topicName");

  if (!this->sdf->HasElement("posAccuracy"))
  {
    ROS_INFO("USBL plugin missing <posAccuracy>, defaults to 0.0");
    this->posAccuracy_ = 0.0;
  }
  else
    this->posAccuracy_ = this->sdf->Get<double>("posAccuracy");

  if (!this->sdf->HasElement("beaconLinkName"))
  {
    ROS_FATAL("USBL plugin missing <baconLinkName>, cannot proceed");
    return;
  }
  else
    this->beacon_link_name_ = this->sdf->Get<std::string>("beaconLinkName");
  if (!this->sdf->HasElement("dunkerLinkName"))
  {
    ROS_FATAL("USBL plugin missing <dunkerLinkName>, cannot proceed");
    return;
  }
  else
    this->dunker_link_name_ = this->sdf->Get<std::string>("dunkerLinkName");

  if (!this->sdf->HasElement("updateRate"))
  {
    ROS_DEBUG("USBL plugin missing <updateRate>, defaults to 1.0");
    this->update_rate_ = 1.0;
  }
  else
    this->update_rate_ = this->sdf->GetElement("updateRate")->Get<double>();

  // Make sure the ROS node for Gazebo has already been initialized
  if (!ros::isInitialized())
  {
    ROS_FATAL_STREAM("A ROS node for Gazebo has not been initialized, unable to load plugin. "
      << "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
    return;
  }

  this->rosnode_ = new ros::NodeHandle(this->robot_namespace_);

  // publish multi queue
  this->pmq.startServiceThread();

  // assert that the body by link_name_ exists
  this->beacon_link = boost::dynamic_pointer_cast<physics::Link>(
    this->world_->GetEntity(this->beacon_link_name_));
  if (!this->beacon_link)
  {
	  //std::cout << "Beacon id: " << this->beacon_link->GetId() << "\n";
    ROS_FATAL("usbl plugin error: beaconLinkName: %s does not exist\n",
      this->beacon_link_name_.c_str());
    return;
  }

  // assert that the body by link_name_ exists
  this->dunker_link = boost::dynamic_pointer_cast<physics::Link>(
    this->world_->GetEntity(this->dunker_link_name_));
  if (!this->dunker_link)
  {
    //std::cout << "Beacon id: " << this->beacon_link->GetId() << "\n";
    ROS_FATAL("usbl plugin error: dunkerLinkName: %s does not exist\n",
      this->dunker_link_name_.c_str());
    return;
  }

  // if topic name specified as empty, do not publish
  if (this->topic_name_ != "")
  {
    this->pub_Queue = this->pmq.addPub<laughlin_uw_plugins::USBL>();
    this->pub_ = this->rosnode_->advertise<laughlin_uw_plugins::USBL>(
      this->topic_name_, 1);

  }

  // Initialize the controller
  this->last_time_ = this->world_->GetSimTime();

  // start custom queue for USBL
  this->callback_queue_thread_ =
    boost::thread(boost::bind(&USBLPlugin::USBLQueueThread, this));


  // New Mechanism for Updating every World Cycle
  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  this->update_connection_ = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&USBLPlugin::UpdateChild, this));
}

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void USBLPlugin::UpdateChild()
{
  common::Time cur_time = this->world_->GetSimTime();

  // rate control
  if (this->update_rate_ > 0 &&
      (cur_time - this->last_time_).Double() < (1.0 / this->update_rate_))
    return;

  if ((this->pub_.getNumSubscribers() > 0 && this->topic_name_ != ""))
  {
	// get transformations to beacon
	this->g_Inertial_USBLBeacon.block<3,3>(0,0) = this->gazMatrix3_EigMatrix3d(this->beacon_link->GetWorldPose().rot.GetAsMatrix3());
	this->g_Inertial_USBLBeacon(0,3) = this->beacon_link->GetWorldPose().pos[0];
	this->g_Inertial_USBLBeacon(1,3) = this->beacon_link->GetWorldPose().pos[1];
	this->g_Inertial_USBLBeacon(2,3) = this->beacon_link->GetWorldPose().pos[2];
	this->g_Inertial_USBLBeacon(3,3) = 1;
	// std::cout << "Hw_b: \n" << this->g_Inertial_USBLBeacon << std::endl;

	// get transformation to dunking transducer
	physics::EntityPtr sensorParentEntity = this->world_->GetEntity(this->sensorParent->ParentName());

	this->g_Inertial_USBLDunker.block<3,3>(0,0) = this->gazMatrix3_EigMatrix3d(this->dunker_link->GetWorldPose().rot.GetAsMatrix3());
	this->g_Inertial_USBLDunker(0,3) = this->dunker_link->GetWorldPose().pos[0];
	this->g_Inertial_USBLDunker(1,3) = this->dunker_link->GetWorldPose().pos[1];
	this->g_Inertial_USBLDunker(2,3) = this->dunker_link->GetWorldPose().pos[2];
	this->g_Inertial_USBLDunker(3,3) = 1;
	// std::cout << "Hw_dunker: \n" << "\n" << this->g_Inertial_USBLDunker << std::endl;

	// find location of beacon in dunking transducer frame
	this->g_tmp = g_Inertial_USBLDunker.inverse() * g_Inertial_USBLBeacon;
	this->v_tmp = g_tmp.block<4,1>(0,3);
	this->v_tmp(3) = 0;

	//compute range
	this->usbl_msg_.range = this->v_tmp.norm();
	this->pos_std_dev_ = this->posAccuracy_ * this->usbl_msg_.range;

	//compute noise and add to position (noise is linear function of slant range)
	std::normal_distribution<double> distribution(0.0,this->pos_std_dev_);


	this->v_tmp(0) = this->v_tmp(0) + distribution(this->generator);
	this->v_tmp(1) = this->v_tmp(1) + distribution(this->generator);
	this->v_tmp(2) = this->v_tmp(2) + distribution(this->generator);

	//recompute range but with added noise
	this->usbl_msg_.range = this->v_tmp.norm();

	//azi = angle measured from x-axis
	this->usbl_msg_.azi = atan2( (double)this->v_tmp(1),(double)this->v_tmp(0) ) * 180/M_PI;

  //wrap it to [0,360)
  this->usbl_msg_.azi = wrapto360(this->usbl_msg_.azi);

	//ele = angle measured from vertical (this is how Posidonia by IXSEA reports,)
	this->usbl_msg_.vert_ang = atan2( (double)this->v_tmp(2), (double)this->v_tmp.head(2).norm() ) * 180/M_PI;

  //wrap it to [0,360)
  this->usbl_msg_.vert_ang = wrapto360(this->usbl_msg_.vert_ang);

  //want to report angle from vertical, not true "elevation measurement"
  this->usbl_msg_.vert_ang = 90 + (360 - this->usbl_msg_.vert_ang);

    //wrap it to [0,360)
  this->usbl_msg_.vert_ang = wrapto360(this->usbl_msg_.vert_ang);

  // Also add XYZ coordinates to USBL message in case user wants to use those instea of spherical coords
  this->usbl_msg_.x = v_tmp(0);
  this->usbl_msg_.y = v_tmp(1);
  this->usbl_msg_.z = v_tmp(2);

  //add time to header
  this->usbl_msg_.header.stamp.sec = this->world_->GetSimTime().sec;
  this->usbl_msg_.header.stamp.nsec = this->world_->GetSimTime().nsec;

	{
      boost::mutex::scoped_lock lock(this->lock_);
      // publish to ros
      if (this->pub_.getNumSubscribers() > 0 && this->topic_name_ != "")
          this->pub_Queue->push(this->usbl_msg_, this->pub_);
    }

    // save last time stamp
    this->last_time_ = cur_time;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Put data to the interface
void USBLPlugin::USBLQueueThread()
{
  static const double timeout = 0.01;

  while (this->rosnode_->ok())
  {
    this->usbl_queue_.callAvailable(ros::WallDuration(timeout));
  }
}

Eigen::Matrix3d USBLPlugin::gazMatrix3_EigMatrix3d(math::Matrix3 inMatrix)
{
	Eigen::Matrix3d m;

	m(0,0) = inMatrix[0][0];
	m(0,1) = inMatrix[0][1];
	m(0,2) = inMatrix[0][2];
	m(1,0) = inMatrix[1][0];
	m(1,1) = inMatrix[1][1];
	m(1,2) = inMatrix[1][2];
	m(2,0) = inMatrix[2][0];
	m(2,1) = inMatrix[2][1];
	m(2,2) = inMatrix[2][2];

	return m;
}

double USBLPlugin::wrapto360(double x){
    x = fmod(x,360);
    if (x < 0)
        x += 360;
    return x;
}
}
