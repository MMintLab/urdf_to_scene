/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2021, Captain Yoshi
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Bielefeld University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Captain Yoshi
   Desc:
*/

#include <urdf_to_scene/scene_parser.h>

#include <eigen_conversions/eigen_msg.h>

#include <geometry_msgs/PoseStamped.h>
#include <geometric_shapes/mesh_operations.h>
#include <geometric_shapes/shape_operations.h>
#include <geometric_shapes/shape_extents.h>
#include <shape_msgs/SolidPrimitive.h>

bool SceneParser::loadURDF(ros::NodeHandle& nh, const std::string& param_name) {
	model_.clear();
	scene_ = moveit_msgs::PlanningScene();

	if (!model_.initParamWithNodeHandle(param_name, nh)) {
		ROS_ERROR("Failed to load URDF from param server : %s", param_name.c_str());
		return false;
	}
	return true;
}

bool SceneParser::loadURDF(const std::string& urdf_str) {
	model_.clear();
	scene_ = moveit_msgs::PlanningScene();

	if (!model_.initString(urdf_str)) {
		ROS_ERROR("Failed to load URDF from string");
		return false;
	}
	return true;
}

void SceneParser::getCollisionObjects(std::vector<moveit_msgs::CollisionObject>& collision_objects) {
	parseURDFmodel();
	collision_objects = scene_.world.collision_objects;
}

const moveit_msgs::PlanningScene& SceneParser::getPlanningScene() {
	parseURDFmodel();
	return scene_;
}

void SceneParser::parseURDFmodel() {
	auto root_link = model_.getRoot();

	// Parse collision geomtery
	if (root_link->collision) {
		Eigen::Isometry3d link_to_collision_tf = Eigen::Isometry3d::Identity();
		urdfPoseToEigenIsometry(root_link->collision->origin, link_to_collision_tf);
		parseCollisionGeometry(root_link, root_link->name, link_to_collision_tf);
	}

	// Parse child links
	std::vector<urdf::LinkSharedPtr> links = root_link->child_links;
	for (const auto& link : links) {
		Eigen::Isometry3d offset = Eigen::Isometry3d::Identity();
		std::map<std::string, std::string> dummy_to_parent_map;

		parseLink(link, offset, dummy_to_parent_map);
	}
}

void SceneParser::parseLink(const urdf::LinkConstSharedPtr& link, const Eigen::Isometry3d& offset,
                            std::map<std::string, std::string>& dummy_link_names) {
	Eigen::Isometry3d frame = Eigen::Isometry3d::Identity();
	// Dummy links or subframes
	// Keep tf offset wrt. a link with collision. The plannig scene dosen't accept dummy collision objects?
	if (!link->collision) {
		urdfPoseToEigenIsometry(link->parent_joint->parent_to_joint_origin_transform, frame);
		frame = offset * frame;

		// Map dummy link to a link with collision
		auto parent_link = link->getParent();
		while (parent_link) {
			if (parent_link->collision) {
				dummy_link_names.insert(std::make_pair(link->name, parent_link->name));
				break;
			}
			parent_link = parent_link->getParent();
		}
		if (!parent_link)
			dummy_link_names.insert(std::make_pair(link->name, model_.getRoot()->name));
	}
	// Calculate parent to joint tf with offset from previous dummy link/s
	else {
		Eigen::Isometry3d parentjoint_to_link_tf = Eigen::Isometry3d::Identity();
		Eigen::Isometry3d link_to_collision_tf = Eigen::Isometry3d::Identity();
		Eigen::Isometry3d parentjoint_to_collision_tf = Eigen::Isometry3d::Identity();

		urdfPoseToEigenIsometry(link->parent_joint->parent_to_joint_origin_transform, parentjoint_to_link_tf);
		urdfPoseToEigenIsometry(link->collision->origin, link_to_collision_tf);

		std::string frame_id = link->getParent()->name;
		if (dummy_link_names.find(frame_id) != dummy_link_names.end()) {
			frame_id = dummy_link_names.find(frame_id)->second;
		}
		// The collision object is added wrt. the frame_id
		parentjoint_to_collision_tf = offset * parentjoint_to_link_tf * link_to_collision_tf;
		parseCollisionGeometry(link, frame_id, parentjoint_to_collision_tf);

		// We want the children links to be wrt. the link frame as opposed to the collsion frame
		frame = link_to_collision_tf.inverse();
	}

	// Parse children links
	std::vector<urdf::LinkSharedPtr> child_links = link->child_links;
	for (const auto& child_link : child_links) {
		parseLink(child_link, frame, dummy_link_names);
	}
}

void SceneParser::parseCollisionGeometry(const urdf::LinkConstSharedPtr& link, const std::string& frame_id,
                                         const Eigen::Isometry3d& parent_to_collision_tf) {
	// TODO: Add cone primitive and plane (Not supported in URDF)
	scene_.world.collision_objects.emplace_back();
	urdf::GeometrySharedPtr geom = link->collision->geometry;

	if (geom->type == geom->BOX) {
		auto box = std::dynamic_pointer_cast<urdf::Box>(geom);
		shape_msgs::SolidPrimitive primitive;
		primitive.type = primitive.BOX;
		primitive.dimensions.resize(3);
		primitive.dimensions = { box->dim.x, box->dim.y, box->dim.z };

		createCollisionObjectPrimitive(scene_.world.collision_objects.back(), link->name, primitive,
		                               parent_to_collision_tf, frame_id);
	} else if (geom->type == geom->CYLINDER) {
		auto cylinder = std::dynamic_pointer_cast<urdf::Cylinder>(geom);
		shape_msgs::SolidPrimitive primitive;
		primitive.type = primitive.CYLINDER;
		primitive.dimensions.resize(2);
		primitive.dimensions = { cylinder->length, cylinder->radius };

		createCollisionObjectPrimitive(scene_.world.collision_objects.back(), link->name, primitive,
		                               parent_to_collision_tf, frame_id);
	} else if (geom->type == geom->SPHERE) {
		auto sphere = std::dynamic_pointer_cast<urdf::Sphere>(geom);
		shape_msgs::SolidPrimitive primitive;
		primitive.type = primitive.SPHERE;
		primitive.dimensions.resize(1);
		primitive.dimensions = { sphere->radius };

		createCollisionObjectPrimitive(scene_.world.collision_objects.back(), link->name, primitive,
		                               parent_to_collision_tf, frame_id);
	} else if (geom->type == geom->MESH) {
		auto mesh = std::dynamic_pointer_cast<urdf::Mesh>(geom);
		shape_msgs::SolidPrimitive primitive;
		const Eigen::Vector3d scaling = Eigen::Vector3d(mesh->scale.x, mesh->scale.y, mesh->scale.z);

		createCollisionObjectMesh(scene_.world.collision_objects.back(), link->name, mesh->filename,
		                          parent_to_collision_tf, frame_id, scaling);
	} else {
		ROS_ERROR("Collision geometry type not supported");
		scene_.world.collision_objects.pop_back();
	}
}

void SceneParser::urdfPoseToEigenIsometry(const urdf::Pose& urdf_pose, Eigen::Isometry3d& eigen_pose) {
	eigen_pose.translate(Eigen::Vector3d(urdf_pose.position.x, urdf_pose.position.y, urdf_pose.position.z));
	Eigen::Quaterniond q;
	q.w() = urdf_pose.rotation.w;
	q.x() = urdf_pose.rotation.x;
	q.y() = urdf_pose.rotation.y;
	q.z() = urdf_pose.rotation.z;

	eigen_pose.linear() = q.matrix();
}

void SceneParser::createCollisionObjectPrimitive(moveit_msgs::CollisionObject& collision_object,
                                                 const std::string& object_id,
                                                 const shape_msgs::SolidPrimitive& primitive,
                                                 const Eigen::Isometry3d& frame, const std::string& frame_id) {
	geometry_msgs::PoseStamped pose_stamped;
	tf::poseEigenToMsg(frame, pose_stamped.pose);
	pose_stamped.header.frame_id = frame_id;

	collision_object.id = object_id;
	collision_object.header = pose_stamped.header;
	collision_object.operation = moveit_msgs::CollisionObject::ADD;
	collision_object.primitives.resize(1);
	collision_object.primitives[0] = primitive;
	collision_object.primitive_poses.resize(1);
	collision_object.primitive_poses[0] = pose_stamped.pose;
}

void SceneParser::createCollisionObjectMesh(moveit_msgs::CollisionObject& collision_object,
                                            const std::string& object_id, const std::string& mesh_path,
                                            const Eigen::Isometry3d& frame, const std::string& frame_id,
                                            const Eigen::Vector3d& scaling) {
	geometry_msgs::PoseStamped pose_stamped;
	tf::poseEigenToMsg(frame, pose_stamped.pose);
	pose_stamped.header.frame_id = frame_id;

	shapes::Shape* shape = shapes::createMeshFromResource(mesh_path, scaling);
	shapes::ShapeMsg shape_msg;
	shapes::constructMsgFromShape(shape, shape_msg);

	collision_object.id = object_id;
	collision_object.header = pose_stamped.header;
	collision_object.operation = moveit_msgs::CollisionObject::ADD;
	collision_object.meshes.resize(1);
	collision_object.meshes[0] = boost::get<shape_msgs::Mesh>(shape_msg);
	collision_object.mesh_poses.resize(1);
	collision_object.mesh_poses[0] = pose_stamped.pose;
}

void SceneParser::printTF(const std::string& tf_name, const Eigen::Isometry3d& tf) {
	std::cout << tf_name << ":" << std::endl;

	std::cout << "xyz" << std::endl;
	std::cout << tf.translation().matrix().x() << " ";
	std::cout << tf.translation().matrix().y() << " ";
	std::cout << tf.translation().matrix().z() << std::endl;

	std::cout << "rpy" << std::endl;
	std::cout << tf.rotation().eulerAngles(2, 1, 0).z() << " ";
	std::cout << tf.rotation().eulerAngles(2, 1, 0).y() << " ";
	std::cout << tf.rotation().eulerAngles(2, 1, 0).x() << std::endl;
}
