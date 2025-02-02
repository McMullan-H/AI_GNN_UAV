/* 
Handles user-requested discrete waypoints and produces the appropriate
reference_state for the controller.
Two modes are supported for waypoints: TIME (0) and SPEED (1).
  TIME(0): generates a trajectory that meets the requested waypoint 
  at exactly t seconds in the future. t is user-provided.
  This is an implementation of the paper:
      Mueller, Mark W., Markus Hehn, and Raffaello D'Andrea.
      "A computationally efficient motion primitive for
      quadrocopter trajectory generation."
      IEEE Transactions on Robotics, Vol. 31.6, pp. 1294-1310, 2015.
  for trajectories that have defined position+velocity start and end states.
  Resultant trajectories are energy-optimal and second-order smooth.

  SPEED(1): generates a linear constant-speed path from current location
  to the requested waypoint. Translational speed target is user-provided.
  
  Overview:
    1. Accept a terminal state [pos3ax,vel3ax,yaw], and, allocated_time or translational_speed;
    2. Snapshot current state
    3. Plan a smooth trajectory towards the destination
    4. Hover in the beginning and end.

  -- aj // May 2020 // Nimbus Lab.
*/

#include <iostream>
#include <chrono>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/float32.hpp"
#include "std_msgs/msg/int8.hpp"

#include "freyja_msgs/msg/waypoint_target.hpp"
#include "freyja_msgs/msg/current_state.hpp"
#include "freyja_msgs/msg/reference_state.hpp"

#include <eigen3/Eigen/Dense>

#define rclcpp_NODE_NAME "waypoint_manager"

#define DEG2RAD(D) ((D)*3.14153/180.0)

const int nAxes = 3;
typedef Eigen::Matrix<double, 1, 3> PosNED;
typedef Eigen::Matrix<double, 1, 6> PosVelNED;
typedef Eigen::Matrix<double, 1, 9> PosVelAccNED;
typedef Eigen::Matrix<double, 3, 3> PosVelAccNED3x3;

typedef freyja_msgs::msg::ReferenceState  ReferenceState;
typedef freyja_msgs::msg::CurrentState    CurrentState;
typedef freyja_msgs::msg::WaypointTarget  WaypointTarget;

using std::placeholders::_1;

enum modes{time_mode=0, speed_mode=1};

class TrajectoryGenerator : public rclcpp::Node
{
  Eigen::Matrix<double, 3, 3> tf_premult_;
  Eigen::Matrix<double, 3, nAxes> albtgm_;
  Eigen::Matrix<double, 3, nAxes> delta_targets_;
  Eigen::Matrix<double, 1, 9> current_state_;
  Eigen::Matrix<double, 3, 3> planning_cur_state_;
  Eigen::Matrix<double, 1, 6> final_state_;

  Eigen::Matrix<double, 1, 3> segment_gradients_;
  Eigen::Matrix<double, 1, 3> segment_intersection_;
  Eigen::Matrix<double, 1, 3> segment_vels_;

  double yaw_target;
  
  /* timing related containers */
  double traj_alloc_duration_;
  double traj_alloc_speed_;
  Eigen::Matrix<double, 3, 3> tnow_matrix_;
  rclcpp::Time t_traj_init_;
  
  bool traj_init_;

  modes wp_mode_;
  double segment_percentage_;
  
  float k_thresh_skipreplan_;

  float init_pn_;
  float init_pe_;
  float init_pd_;
  float init_yaw_;
  
  int terminal_style_; // 0: posn+vel, 1: posn+vel+acc
  
  inline void update_albtgm( const double& );
  inline void update_premult_matrix( const double& );
  inline void trigger_replan_time( const double& );
  inline void trigger_replan_speed( const double& );
  
  public:
    TrajectoryGenerator();

    // requested final state
    rclcpp::Subscription<WaypointTarget>::SharedPtr waypoint_sub_;
    void waypointCallback( const WaypointTarget::ConstSharedPtr );
    
    // vehicle current state (from state_manager)
    rclcpp::Subscription<CurrentState>::SharedPtr current_state_sub_;
    void currentStateCallback( const CurrentState::ConstSharedPtr );
    
    rclcpp::Publisher<ReferenceState>::SharedPtr traj_ref_pub_;
    ReferenceState traj_ref_;
    rclcpp::TimerBase::SharedPtr traj_timer_;
    void trajectoryReference();
    
    void publishHoverReference();
};

TrajectoryGenerator::TrajectoryGenerator() : rclcpp::Node( rclcpp_NODE_NAME )
{
  /* Operational constants */
  k_thresh_skipreplan_ = 0.10;       // don't replan if new point within this radius

  /* initial location for hover */
  declare_parameter<float>( "init_pn", 0.0 );
  declare_parameter<float>( "init_pe", 0.0 );
  declare_parameter<float>( "init_pd", -0.75 );
  declare_parameter<float>( "init_yaw", 0.0 );

  get_parameter( "init_pn", init_pn_ );
  get_parameter( "init_pe", init_pe_ );
  get_parameter( "init_pd", init_pd_ );
  get_parameter( "init_yaw", init_yaw_ );

  /* unused argument for trajectory shaping */
  declare_parameter<int>( "term_style", int(1) );

  /* used for speed and duration constraints*/
  traj_alloc_duration_ = 10.0;
  traj_alloc_speed_ = 1.0;

  traj_init_ = false;

  // default to speed mode (as per waypoint default)
  wp_mode_ = speed_mode;

  // Init eigen matrices 
  current_state_ = Eigen::Matrix<double, 1, 9>::Zero();
  update_premult_matrix( traj_alloc_duration_ );
  
  /* Subscriptions */
  current_state_sub_ = create_subscription<CurrentState> ( "current_state", 1,
                           std::bind(&TrajectoryGenerator::currentStateCallback, this, _1) );
  waypoint_sub_ = create_subscription<WaypointTarget>( "discrete_waypoint_target", 1, 
                           std::bind(&TrajectoryGenerator::waypointCallback, this, _1) );

  /* Publishers */
  traj_ref_pub_ = create_publisher<ReferenceState>( "reference_state", 1 );

  /* Fixed-rate trajectory provider. Ensure ~40-50hz. */
  float traj_period = 1.0/50.0;
  traj_timer_ = rclcpp::create_timer( this, get_clock(), std::chrono::duration<float>(traj_period),
                            std::bind(&TrajectoryGenerator::trajectoryReference, this) );

  RCLCPP_WARN( get_logger(), "Initialized; waiting for waypoint .." );
}

void TrajectoryGenerator::currentStateCallback( const CurrentState::ConstSharedPtr msg )
{
  /* make current_state available locally */
  current_state_.head<6>() = Eigen::Map<const PosVelNED>( msg->state_vector.data() );
}

void TrajectoryGenerator::waypointCallback( const WaypointTarget::ConstSharedPtr msg )
{
  /*
    WaypointTarget:
      [terminal_pn, terminal_pe, terminal_pd, terminal_vn, terminal_ve, terminal_vd, terminal_yaw]
      allocated_time
      translational_speed
      waypoint_mode (WaypointTarget::TIME=0, WaypointTarget::SPEED=1)
  */
  
  // Check if we aren't in a correct mode (time or speed mode)
  if( msg->waypoint_mode != msg->TIME && msg->waypoint_mode != msg->SPEED )
  {
    // Reject waypoint
    RCLCPP_WARN( get_logger(), "Waypoint mode incorrect. Ignoring!" );
    return;
  }
  
  // guess if provided waypoint mode was "accidental" or maybe wrong
  if( msg->waypoint_mode == msg->TIME && msg->allocated_time < 0.01 )
  {
    RCLCPP_WARN( get_logger(), "Allocated time too small (possibly zero). Ignoring!" );
    RCLCPP_WARN( get_logger(), "---- Did you mean to use SPEED mode?" );
    return;
  }
  
  if( msg->waypoint_mode == msg->SPEED && msg->translational_speed < 0.001 )
  {
    RCLCPP_WARN( get_logger(), "Translational speed too small (possibly zero). Ignoring!" );
    RCLCPP_WARN( get_logger(), "---- Speed must be positive, and > 0.001 m/s." );
    return;
  }

  // PROCESS WAYPOINT
  // Update yaw
  yaw_target = msg->terminal_yaw;

  // check if the change is big enough to do a replan: final_state_: [1x6]
  Eigen::Matrix<double, 1, 6> updated_final_state;
  updated_final_state << msg->terminal_pn, msg->terminal_pe, msg->terminal_pd,
                         msg->terminal_vn, msg->terminal_ve, msg->terminal_vd;

  if( ( updated_final_state - final_state_ ).norm() > k_thresh_skipreplan_ || !traj_init_ )
  {
    /* accept new waypoint */
    final_state_ = updated_final_state;

    if (msg->waypoint_mode == msg->TIME) // Time mode
    {
      traj_alloc_duration_ = msg->allocated_time;
      trigger_replan_time( traj_alloc_duration_ );
      wp_mode_ = time_mode;
    }
    else                                // Speed mode 
    {
      traj_alloc_speed_ = msg->translational_speed;
      trigger_replan_speed( traj_alloc_speed_ );
      wp_mode_ = speed_mode;
    }
    
    RCLCPP_INFO( get_logger(), "Plan generated!" );
    t_traj_init_ = now();
  }
  else
  {
    // Reject waypoint because it is not distinct enough
    RCLCPP_WARN( get_logger(), "New WP too close to old WP. Ignoring!" );
  }

  // this is only handled once - trajectory is never reinit, only updated.
  if( traj_init_ == false )
  {
    traj_init_ = true;
  }
}

void TrajectoryGenerator::trigger_replan_speed( const double &speed )
{
  // Get current and target position
  auto targetpos = final_state_.head<3>();
  auto currentpos = current_state_.head<3>();

  // x = x0 + (x1 - x0)t ; y = y0 + (y1 - y0)t ; z = z0 + (z1 - z0)t
  segment_gradients_ = targetpos - currentpos;
  segment_intersection_ = currentpos;

  // Calculate length
  double segment_length = (targetpos - currentpos).norm();

  segment_percentage_ = speed / segment_length;
  segment_vels_ = segment_gradients_ * segment_percentage_;
}

void TrajectoryGenerator::trigger_replan_time( const double &dt )
{
  /* TWO STEPS:
     1. Take current snapshot
        Deltas are shaped as: [px py pz; vx vy vz].
  */ 
  auto targetpos = final_state_.head<3>();
  auto targetvel = final_state_.tail<3>();
  auto currentpos = current_state_.head<3>();
  auto currentvel = current_state_.block<1,3>(0,3);
  
  delta_targets_ << targetpos - currentpos - dt*currentvel,
                    targetvel - currentvel,
                    0.0, 0.0, 0.0;
  /* save snapshot of current_state */
  planning_cur_state_ << currentpos,
                         currentvel,
                         0.0, 0.0, 0.0;
  
  
  // 2. update timing parameters
  update_premult_matrix( dt );
}



inline void TrajectoryGenerator::update_premult_matrix( const double &tf )
{
  tf_premult_ <<  720.0,      -360.0*tf,        60.0*tf*tf,
                 -360.0*tf,    168.0*tf*tf,    -24.0*tf*tf*tf,
                  60.0*tf*tf, -24.0*tf*tf*tf,   3.0*tf*tf*tf*tf;
                 
  // necessarily update alpha, beta and gamma as well
  update_albtgm( tf );
}

inline void TrajectoryGenerator::update_albtgm( const double &dt )
{
  /* albtgm_ is stored as three stacked cols of:
          [alpha; beta; gamma] .. for each axis
  */
  albtgm_ = 1.0/std::pow(dt,5) * tf_premult_ * delta_targets_;
}

void TrajectoryGenerator::trajectoryReference( )
{
  /* This is the trajectory generator - gets called at a fixed rate.
  This must keep track of current time since "go". If go-signal has
  not been recorded yet, we must stay at initial position.
  */
    
  float tnow = (now() - t_traj_init_).seconds();
  
  static PosVelAccNED3x3 tref; // [pn, pe, pd;; vn, ve, vd;; an, ae, ad]

  if ( wp_mode_ == time_mode ) // Time mode
  {
    /* check if we are past the final time */
    if( tnow > traj_alloc_duration_ || !traj_init_ )
    {
      publishHoverReference();
      return;
    }
      
    double t5 = std::pow( tnow, 5 )/120.0;
    double t4 = std::pow( tnow, 4 )/24.0;
    double t3 = std::pow( tnow, 3 )/6.0;
    double t2 = std::pow( tnow, 2 )/2.0;
    
    tnow_matrix_ << t5, t4, t3,
                    t4, t3, t2,
                    t3, t2, tnow;  
    // three stacked cols of [pos, vel, acc]^T for each axis.
    tref = tnow_matrix_ * albtgm_ +
              (Eigen::Matrix<double, 3, 3>() <<
                                1, tnow, t2,
                                0,  1, tnow,
                                0,  0,   1).finished() * planning_cur_state_;
  }
  else // Speed mode
  {
    /* check if we have arrived at the waypoint */
    if( ( tnow * segment_percentage_ ) >= 1  || !traj_init_ )
    {
      publishHoverReference();
      return;
    }

    // Calculate new position based on line
    auto new_pos = segment_gradients_ * (tnow * segment_percentage_) + 
                   segment_intersection_;

    tref << new_pos,
            segment_vels_,
            0.0, 0.0, 0.0;

  }
  
  /* fill in and publish */
  traj_ref_.pn = tref(0,0);
  traj_ref_.pe = tref(0,1);
  traj_ref_.pd = tref(0,2);

  traj_ref_.vn = tref(1,0);
  traj_ref_.ve = tref(1,1);
  traj_ref_.vd = tref(1,2);

  traj_ref_.an = tref(2,0);
  traj_ref_.ae = tref(2,1);
  traj_ref_.ad = tref(2,2);

  traj_ref_.yaw = yaw_target;     // nothing special for yaw
  traj_ref_.header.stamp = now();

  traj_ref_pub_ -> publish( traj_ref_ );
}

void TrajectoryGenerator::publishHoverReference()
{
  if( !traj_init_ )
  {
    // trajectory hasn't been initialsed
    traj_ref_.pn = init_pn_;
    traj_ref_.pe = init_pe_;
    traj_ref_.pd = init_pd_;
    traj_ref_.yaw = init_yaw_;
  }
  else
  {
    // trajectory segment completed
    traj_ref_.pn = final_state_[0];
    traj_ref_.pe = final_state_[1];
    traj_ref_.pd = final_state_[2];
    traj_ref_.yaw = yaw_target;
  }
  
  traj_ref_.vn = 0.0;
  traj_ref_.ve = 0.0;
  traj_ref_.vd = 0.0;

  traj_ref_.an = 0.0;
  traj_ref_.ae = 0.0;
  traj_ref_.ad = 0.0;

  traj_ref_.header.stamp = now();
  traj_ref_pub_ -> publish( traj_ref_ );
}

int main( int argc, char** argv )
{
  rclcpp::init( argc, argv );
  rclcpp::spin( std::make_shared<TrajectoryGenerator>() );
  rclcpp::shutdown();
  return 0;
}


//TODO: Add status waypoint_done, protect trajectory-gen with mutex
//TODO: change to enum class, and start enumerating at 1
