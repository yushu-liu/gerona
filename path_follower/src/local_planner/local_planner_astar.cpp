/// HEADER
#include <path_follower/local_planner/local_planner_astar.h>

/// PROJECT
#include <path_follower/pathfollower.h>

LocalPlannerAStar::LocalPlannerAStar(PathFollower &follower,
                                 tf::Transformer& transformer,
                                 const ros::Duration& update_interval)
    : LocalPlanner(follower, transformer), last_update_(0), update_interval_(update_interval)
{

}

void LocalPlannerAStar::setGlobalPath(Path::Ptr path)
{
    LocalPlanner::setGlobalPath(path);
}

Path::Ptr LocalPlannerAStar::updateLocalPath(const std::vector<Constraint::Ptr>& constraints,
                                             const std::vector<Scorer::Ptr>& scorer,
                                             const std::vector<bool>& fconstraints,
                                             const std::vector<double>& wscorer)
{
    // this planner uses the A* search algorithm

    ros::Time now = ros::Time::now();
    Stopwatch gsw;
    gsw.restart();

    // only calculate a new local path, if enough time has passed.
    // TODO: also replan for other reasons, e.g. the global path has changed, ...
    if(last_update_ + update_interval_ < now) {
        // only look at the first sub path for now
        Stopwatch sw;
        sw.restart();
        auto waypoints = (SubPath) global_path_;
        initIndexes();
        int path_ini = sw.usElapsed();

        // calculate the corrective transformation to map from world coordinates to odom
        if(!transformer_.waitForTransform("map", "odom", now, ros::Duration(0.1))) {
            ROS_WARN_THROTTLE_NAMED(1, "local_path", "cannot transform map to odom");
            return nullptr;
        }

        tf::StampedTransform now_map_to_odom;
        transformer_.lookupTransform("map", "odom", now, now_map_to_odom);

        tf::Transform transform_correction = now_map_to_odom.inverse();

        sw.restart();
        // transform the waypoints from world to odom
        for(Waypoint& wp : waypoints) {
            tf::Point pt(wp.x, wp.y, 0);
            pt = transform_correction * pt;
            wp.x = pt.x();
            wp.y = pt.y();

            tf::Quaternion rot = tf::createQuaternionFromYaw(wp.orientation);
            rot = transform_correction * rot;
            wp.orientation = tf::getYaw(rot);
        }
        int trans_t = sw.usElapsed();

        sw.restart();
        if(fconstraints.at(0)){
            std::dynamic_pointer_cast<Dis2Path_Constraint>(constraints.at(0))->setSubPath(waypoints,
                                                                                          index1, index2);
        }
        if(fconstraints.at(1)){
            if(last_local_path_.empty()){
                std::dynamic_pointer_cast<Dis2Path_Constraint>(constraints.at(1))->setSubPath(last_local_path_,
                                                                                          last_local_path_.size()-1, 0);
            }else{
                std::dynamic_pointer_cast<Dis2Path_Constraint>(constraints.at(1))->setSubPath(last_local_path_,
                                                                                          0, last_local_path_.size()-1);
            }
        }
        if(wscorer.at(0) != 0.0){
            std::dynamic_pointer_cast<Dis2Start_Scorer>(scorer.at(0))->setPath(waypoints, c_dist,
                                                                               index1, index2);
        }
        if(wscorer.at(1) != 0.0){
            std::dynamic_pointer_cast<Dis2Path_Scorer>(scorer.at(1))->setSubPath(waypoints,
                                                                                 index1, index2);
        }
        if(wscorer.at(3) != 0.0){
            if(last_local_path_.empty()){
                std::dynamic_pointer_cast<Dis2Path_Scorer>(scorer.at(3))->setSubPath(last_local_path_,
                                                                                 last_local_path_.size()-1, 0);
            }else{
                std::dynamic_pointer_cast<Dis2Path_Scorer>(scorer.at(3))->setSubPath(last_local_path_,
                                                                                 0, last_local_path_.size()-1);
            }
        }
        int cs_ini = sw.usElapsed();

        // find the subpath that starts closest to the robot
        Eigen::Vector3d pose = follower_.getRobotPose();

        const Waypoint& last = waypoints.back();
        //const tf::Point lastp(last.x,last.y,last.orientation);
        const tf::Point wposep(pose(0),pose(1),pose(2));

        //float dis2last = (wscorer.at(0) != 0.0)?(wscorer.at(0)*scorer.at(0)->score(lastp)):0.0;
        float dis2last = (wscorer.at(0) != 0.0)?global_path_.s(global_path_.n()-1):0.0;

        double heuristic = (dis2last - ((wscorer.at(0) != 0.0)?(wscorer.at(0)*scorer.at(0)->score(wposep)):0.0))
                                + ((wscorer.at(1) != 0.0)?(wscorer.at(1)*scorer.at(1)->score(wposep)):0.0)
                                + ((wscorer.at(2) != 0.0)?(wscorer.at(2)*scorer.at(2)->score(wposep)):0.0)
                                + ((fconstraints.at(1)?constraints.at(1)->isSatisfied(wposep):true)?
                                       ((wscorer.at(3) != 0.0)?(wscorer.at(3)*scorer.at(3)->score(wposep)):0.0):0.0);

        HNode wpose(pose(0),pose(1),pose(2),nullptr,0);
        wpose.gScore_ = 0.0;
        wpose.fScore_ = heuristic;

        if(dis2last - ((wscorer.at(0) != 0.0)?(wscorer.at(0)*scorer.at(0)->score(wposep)):0.0) < 0.8){
            return nullptr;
        }

        std::vector<HNode> nodes(200);

        std::vector<HNode*> closedSet;

        nodes.at(0) = wpose;

        prio_queue openSet;
        openSet.insert(&nodes[0]);
        double go_dist = std::numeric_limits<double>::infinity();
        HNode* obj = nullptr;
        int li_level = 10;
        int nnodes = 1;

        HNode* current;
        int init_t = gsw.usElapsed();

        while(!openSet.empty() && (openSet.empty()?nodes.back().level_:(*openSet.begin())->level_) <= li_level){
            current = *openSet.begin();
            openSet.erase(openSet.begin());
            if(isNearEnough(*current,last)){
                obj = current;
                break;
            }
            closedSet.push_back(current);

            std::vector<HNode*> successors;
            getSuccessors(current, nnodes, successors, nodes, constraints, fconstraints, true);
            for(std::size_t i = 0; i < successors.size(); ++i){
                if(std::find(closedSet.begin(), closedSet.end(), successors[i]) != closedSet.end()){
                    continue;
                }

                double tentative_gScore = current->gScore_ + 0.15;//vllt tat. Abstand?

                if(tentative_gScore >= successors[i]->gScore_){
                    continue;
                }

                successors[i]->parent_ = current;
                successors[i]->gScore_ = tentative_gScore;

                const tf::Point processed(successors[i]->x,successors[i]->y,
                        successors[i]->orientation);

                heuristic = (dis2last - ((wscorer.at(0) != 0.0)?(wscorer.at(0)*scorer.at(0)->score(processed)):0.0))
                        + ((wscorer.at(1) != 0.0)?(wscorer.at(1)*scorer.at(1)->score(processed)):0.0)
                        + ((wscorer.at(2) != 0.0)?(wscorer.at(2)*scorer.at(2)->score(processed)):0.0)
                        + ((fconstraints.at(1)?constraints.at(1)->isSatisfied(processed):true)?
                               ((wscorer.at(3) != 0.0)?(wscorer.at(3)*scorer.at(3)->score(processed)):0.0):0.0);


                successors[i]->fScore_ = heuristic;

                prio_queue::const_iterator inOpen = std::find(openSet.begin(), openSet.end(), successors[i]);
                if(inOpen == openSet.end()){
                    openSet.insert(successors[i]);
                }else{
                    openSet.erase(inOpen);
                    openSet.insert(successors[i]);
                }

                if(heuristic < go_dist){
                    go_dist = heuristic;
                    obj = successors[i];
                }
            }
        }

        std::vector<Waypoint> local_wps;
        int pos_t;
        if(obj != nullptr){
            global_path_.set_s_new(global_path_.s_new() + 0.7);
            LNode* cu = obj;
            while(cu != nullptr){
                local_wps.push_back(*cu);
                cu = cu->parent_;
            }
            std::reverse(local_wps.begin(),local_wps.end());
            last_local_path_.assign(local_wps.begin(),local_wps.end());
            //smoothing
            sw.restart();
            local_wps = smoothPath(local_wps, 0.6, 0.15);
            //interpolate
            local_wps = interpolatePath(local_wps, 0.1);
            //final smoothing
            local_wps = smoothPath(local_wps, 2.0, 0.4);
            pos_t = sw.usElapsed();
        }else{
            return nullptr;
        }

        Path::Ptr local_path(new Path("/odom"));
        local_path->setPath({local_wps});

        follower_.getController()->reset();
        follower_.getController()->setPath(local_path);

        last_update_ = now;
        int end_t = gsw.usElapsed();

        ROS_INFO_STREAM("Path inizalitation took " << path_ini << " us");
        ROS_INFO_STREAM("Transformation to /odom took " << trans_t << " us");
        ROS_INFO_STREAM("Constraints/Scorers inizalitation took " << cs_ini << " us");
        ROS_INFO_STREAM("General inizalitation took " << init_t << " us");
        ROS_INFO_STREAM("# Nodes: " << nnodes);
        for(std::size_t i = 0; i < constraints.size(); ++i){
            if(fconstraints.at(i)){
                ROS_INFO_STREAM("Constraint #" << (i+1) << " took " << constraints.at(i)->nsUsed()/1000.0 << " us");
            }
        }
        for(std::size_t i = 0; i < scorer.size(); ++i){
            if(wscorer.at(i) != 0.0){
                ROS_INFO_STREAM("Scorer #" << (i+1) << " took " << scorer.at(i)->nsUsed()/1000.0 << " us");
            }
        }
        ROS_INFO_STREAM("Local path postprocessing took " << pos_t << " us");
        ROS_INFO_STREAM("Local Planner duration: " << (end_t/1000.0) << " ms");

        return local_path;

    } else {
        return nullptr;
    }
}
