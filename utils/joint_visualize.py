import matplotlib.pyplot as plt

# Define robot parameters
robot_width = 0.5  # meters
robot_length = 0.7  # meters
leg_length_1 = [0.2, 0.2, 0.2, 0.2]  # length of the first segment of each leg
leg_length_2 = [0.3, 0.3, 0.3, 0.3]  # length of the second segment of each leg

# Function to read joint angles from file
def read_joint_angles(file_path):
    with open(file_path, 'r') as file:
        lines = file.readlines()
    
    times = []
    joint_angles = []
    
    for line in lines:
        values = list(map(float, line.split()))
        times.append(values[0])
        joint_angles.append(values[1:])
    
    return times, joint_angles

# Function to plot joint angles
def plot_joint_angles(times, joint_angles):
    num_joints = len(joint_angles[0])
    fig, axs = plt.subplots(num_joints, 1, figsize=(10, num_joints * 2), sharex=True)
    
    for i in range(num_joints):
        axs[i].plot(times, [angle[i] for angle in joint_angles], label=f'Joint {i+1}')
        axs[i].set_ylabel('Angle (rad)')
        axs[i].legend()
    
    axs[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    plt.show()

# Path to the joint angles file
file_path = 'mini_cheetah_joint_new.txt'

# Read and plot joint angles
times, joint_angles = read_joint_angles(file_path)
plot_joint_angles(times, joint_angles)

