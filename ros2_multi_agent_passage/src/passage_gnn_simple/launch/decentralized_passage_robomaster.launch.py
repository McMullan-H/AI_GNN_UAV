from launch import LaunchDescription
from launch.launch_description_sources import AnyLaunchDescriptionSource
from launch_ros.actions import Node
from launch.actions import IncludeLaunchDescription


def generate_launch_description():
    uuids = [
        "uav_0",
        "uav_1",
        "uav_2",
        "uav_3",
        "uav_4",
    ]

    ld = [
        IncludeLaunchDescription(
            AnyLaunchDescriptionSource(
                "launch/rvo_robomaster.launch.yaml"
            )
        ),
        Node(
            package="evaluation_infrastructure",
            executable="pose_state_server",
            parameters=[{
                "episodes_path": "src/passage_gnn_simple/config/passage_8e2d2_5_agents.yaml",
                "n_agents": len(uuids),
                "n_episodes": 10,
                "n_trials": 3,
            }],
            # output="screen",
        )
    ]
    
    # 8e2d2_4899.pt

    for uuid in uuids:
        ld.append(
            Node(
                package="passage_gnn_simple",
                executable="decentralized_passage",
                namespace=uuid,
                parameters=[{
                    "model_path": "src/passage_gnn_simple/models/model.pt",
                    "uuid": uuid,
                    "cycle_frequency": 20,
                    "max_v": 1.5,
                    "max_a": 1.0,
                }],
                output="screen",
                emulate_tty=True,
            )
        )
    return LaunchDescription(ld)
