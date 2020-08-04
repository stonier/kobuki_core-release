/**
 * @file /kobuki_driver/src/tools/simple_keyop.cpp
 *
 * @brief Tools/utility program to control robot by keyboard.
 *
 * License: BSD
 *   https://raw.githubusercontent.com/kobuki-base/kobuki_core/license/LICENSE
 **/

/*****************************************************************************
 ** Includes
 *****************************************************************************/

#include <string>
#include <csignal>
#include <termios.h> // for keyboard input
#include <ecl/command_line.hpp>
#include <ecl/time.hpp>
#include <ecl/threads.hpp>
#include <ecl/sigslots.hpp>
#include <ecl/exceptions.hpp>
#include <ecl/linear_algebra.hpp>
#include <ecl/geometry/legacy_pose2d.hpp>
#include "kobuki_driver/kobuki.hpp"

/*****************************************************************************
** Classes
*****************************************************************************/

/**
 * @brief Keyboard remote control for our robot core (mobile base).
 *
 */
class KobukiManager
{
public:
  /*********************
   ** C&D
   **********************/
  KobukiManager();
  ~KobukiManager();
  bool init(const std::string & device);

  /*********************
   ** Runtime
   **********************/
  void spin();

  /*********************
   ** Callbacks
   **********************/
  void processStreamData();

  /*********************
   ** Accessor
   **********************/
  ecl::LegacyPose2D<double> getPose();

private:
  double vx, wz;
  ecl::LegacyPose2D<double> pose;
  kobuki::Kobuki kobuki;

  double linear_vel_step, linear_vel_max;
  double angular_vel_step, angular_vel_max;
  std::string name;
  ecl::Slot<> slot_stream_data;

  /*********************
   ** Commands
   **********************/
  void incrementLinearVelocity();
  void decrementLinearVelocity();
  void incrementAngularVelocity();
  void decrementAngularVelocity();
  void resetVelocity();

  /*********************
   ** Keylogging
   **********************/

  void keyboardInputLoop();
  void processKeyboardInput(char c);
  void restoreTerminal();
  bool quit_requested;
  int key_file_descriptor;
  struct termios original_terminal_state;
  ecl::Thread thread;
};

/*****************************************************************************
 ** Implementation
 *****************************************************************************/

/**
 * @brief Default constructor, needs initialisation.
 */
KobukiManager::KobukiManager() :
  vx(0.0), wz(0.0),
  linear_vel_step(0.05),
  linear_vel_max(1.0),
  angular_vel_step(0.33),
  angular_vel_max(6.6),
  slot_stream_data(&KobukiManager::processStreamData, *this),
  quit_requested(false),
  key_file_descriptor(0)
{
  tcgetattr(key_file_descriptor, &original_terminal_state); // get terminal properties
}

KobukiManager::~KobukiManager()
{
  kobuki.setBaseControl(0,0); // linear_velocity, angular_velocity in (m/s), (rad/s)
  kobuki.disable();
  tcsetattr(key_file_descriptor, TCSANOW, &original_terminal_state);
}

/**
 * @brief Initialises the node.
 */
bool KobukiManager::init(const std::string & device)
{
  /*********************
   ** Parameters
   **********************/
  std::cout << "KobukiManager : using linear  vel step [" << linear_vel_step << "]." << std::endl;
  std::cout << "KobukiManager : using linear  vel max  [" << linear_vel_max << "]." << std::endl;
  std::cout << "KobukiManager : using angular vel step [" << angular_vel_step << "]." << std::endl;
  std::cout << "KobukiManager : using angular vel max  [" << angular_vel_max << "]." << std::endl;

  /*********************
   ** Velocities
   **********************/
  vx = 0.0;
  wz = 0.0;

  /*********************
   ** Kobuki
   **********************/
  kobuki::Parameters parameters;
  parameters.sigslots_namespace = "/kobuki";
  parameters.device_port = device;
  parameters.enable_acceleration_limiter = true;

  kobuki.init(parameters);
  kobuki.enable();
  slot_stream_data.connect("/kobuki/stream_data");

  /*********************
   ** Wait for connection
   **********************/
  thread.start(&KobukiManager::keyboardInputLoop, *this);
  return true;
}

/*****************************************************************************
 ** Implementation [Spin]
 *****************************************************************************/

/**
 * @brief Worker thread loop; sends current velocity command at a fixed rate.
 *
 * It also process ros functions as well as aborting when requested.
 */
void KobukiManager::spin()
{
/*
  {
    // just in case we got here not via a keyboard quit request
    quit_requested = true;
    thread.cancel();
  }
*/
  ecl::Sleep sleep(ecl::Duration(0.1));
  while (!quit_requested){
    sleep();
  }
  thread.join();
}

/*****************************************************************************
 ** Implementation [Keyboard]
 *****************************************************************************/

/**
 * @brief The worker thread function that accepts input keyboard commands.
 *
 * This is ok here - but later it might be a good idea to make a node which
 * posts keyboard events to a topic. Recycle common code if used by many!
 */
void KobukiManager::keyboardInputLoop()
{
  struct termios raw;
  memcpy(&raw, &original_terminal_state, sizeof(struct termios));

  raw.c_lflag &= ~(ICANON | ECHO);
  // Setting a new line, then end of file
  raw.c_cc[VEOL] = 1;
  raw.c_cc[VEOF] = 2;
  tcsetattr(key_file_descriptor, TCSANOW, &raw);

  puts("Reading from keyboard");
  puts("---------------------------");
  puts("Forward/back arrows : linear velocity incr/decr.");
  puts("Right/left arrows : angular velocity incr/decr.");
  puts("Spacebar : reset linear/angular velocities.");
  puts("q : quit.");
  char c;
  while (!quit_requested)
  {
    if (read(key_file_descriptor, &c, 1) < 0)
    {
      perror("read char failed():");
      exit(-1);
    }
    processKeyboardInput(c);
  }
}

/**
 * @brief Process individual keyboard inputs.
 *
 * @param c keyboard input.
 */
void KobukiManager::processKeyboardInput(char c)
{
  /*
   * Arrow keys are a bit special, they are escape characters - meaning they
   * trigger a sequence of keycodes. In this case, 'esc-[-Keycode_xxx'. We
   * ignore the esc-[ and just parse the last one. So long as we avoid using
   * the last one for its actual purpose (e.g. left arrow corresponds to
   * esc-[-D) we can keep the parsing simple.
   */
  switch (c)
  {
    case 68://kobuki_msgs::KeyboardInput::KEYCODE_LEFT:
    {
      incrementAngularVelocity();
      break;
    }
    case 67://kobuki_msgs::KeyboardInput::KEYCODE_RIGHT:
    {
      decrementAngularVelocity();
      break;
    }
    case 65://kobuki_msgs::KeyboardInput::KEYCODE_UP:
    {
      incrementLinearVelocity();
      break;
    }
    case 66://kobuki_msgs::KeyboardInput::KEYCODE_DOWN:
    {
      decrementLinearVelocity();
      break;
    }
    case 32://kobuki_msgs::KeyboardInput::KEYCODE_SPACE:
    {
      resetVelocity();
      break;
    }
    case 'q':
    {
      quit_requested = true;
      break;
    }
    default:
    {
      break;
    }
  }
}

/*****************************************************************************
 ** Implementation [Commands]
 *****************************************************************************/

/**
 * @brief If not already maxxed, increment the command velocities..
 */
void KobukiManager::incrementLinearVelocity()
{
  if (vx <= linear_vel_max)
  {
    vx += linear_vel_step;
  }
//  ROS_INFO_STREAM("KeyOp: linear  velocity incremented [" << cmd->linear.x << "|" << cmd->angular.z << "]");
}

/**
 * @brief If not already minned, decrement the linear velocities..
 */
void KobukiManager::decrementLinearVelocity()
{
  if (vx >= -linear_vel_max)
  {
    vx -= linear_vel_step;
  }
//  ROS_INFO_STREAM("KeyOp: linear  velocity decremented [" << cmd->linear.x << "|" << cmd->angular.z << "]");
}

/**
 * @brief If not already maxxed, increment the angular velocities..
 */
void KobukiManager::incrementAngularVelocity()
{
  if (wz <= angular_vel_max)
  {
    wz += angular_vel_step;
  }
//  ROS_INFO_STREAM("KeyOp: angular velocity incremented [" << cmd->linear.x << "|" << cmd->angular.z << "]");
}

/**
 * @brief If not already mined, decrement the angular velocities..
 */
void KobukiManager::decrementAngularVelocity()
{
  if (wz >= -angular_vel_max)
  {
    wz -= angular_vel_step;
  }
//    ROS_INFO_STREAM("KeyOp: angular velocity decremented [" << cmd->linear.x << "|" << cmd->angular.z << "]");
}

void KobukiManager::resetVelocity()
{
  vx = 0.0;
  wz = 0.0;
//    ROS_INFO_STREAM("KeyOp: reset linear/angular velocities.");
}

void KobukiManager::processStreamData() {
  ecl::LegacyPose2D<double> pose_update;
  ecl::linear_algebra::Vector3d pose_update_rates;
  kobuki.updateOdometry(pose_update, pose_update_rates);
  pose *= pose_update;
//  dx += pose_update.x();
//  dth += pose_update.heading();
  //std::cout << dx << ", " << dth << std::endl;
  //std::cout << kobuki.getHeading() << ", " << pose.heading() << std::endl;
  //std::cout << "[" << pose.x() << ", " << pose.y() << ", " << pose.heading() << "]" << std::endl;

  kobuki.setBaseControl(vx, wz);
}

ecl::LegacyPose2D<double> KobukiManager::getPose() {
  return pose;
}

/*****************************************************************************
** Signal Handler
*****************************************************************************/

bool shutdown_req = false;
void signalHandler(int /* signum */) {
  shutdown_req = true;
}

/*****************************************************************************
** Main
*****************************************************************************/

int main(int argc, char** argv)
{
  ecl::CmdLine cmd_line("simple_keyop program", ' ', "0.2");
  ecl::UnlabeledValueArg<std::string> device_port("device_port", "Path to device file of serial port to open, connected to the kobuki", false, "/dev/kobuki", "string");
  cmd_line.add(device_port);
  cmd_line.parse(argc, argv);

  signal(SIGINT, signalHandler);

  std::cout << "Simple Keyop : Utility for driving kobuki by keyboard." << std::endl;
  KobukiManager kobuki_manager;
  kobuki_manager.init(device_port.getValue());

  ecl::Sleep sleep(1);
  ecl::LegacyPose2D<double> pose;
  try {
    while (!shutdown_req){
      sleep();
      pose = kobuki_manager.getPose();
      std::cout << "current pose: [" << pose.x() << ", " << pose.y() << ", " << pose.heading() << "]" << std::endl;
    }
  } catch ( ecl::StandardException &e ) {
    std::cout << e.what();
  }
  return 0;
}
