/*
 * File:   quad_main.c
 * Author: Aaron
 *
 * Created on August 3, 2022, 2:40 PM
 */

/*******************************************************************************
 * #INCLUDES                                                                   *
 ******************************************************************************/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "xc.h"
#include "Board.h"
#include "SerialM32.h"
#include "System_timer.h"
#include "Radio_serial.h"
#include "common/mavlink.h"
#include "RC_RX.h"
#include "RC_servo.h"
#include "ICM_20948.h"
#include "AHRS.h"
#include "PID.h"




/*******************************************************************************
 * #DEFINES                                                                    *
 ******************************************************************************/
#define HEARTBEAT_PERIOD 1000 //1 sec interval for hearbeat update
#define ANGULAR_RATE_CONTROL_PERIOD 20 //Period for control loop in msec
#define ANGLE_CONTROL_PERIOD 20 // msec for calculating new angular control output
#define BUFFER_SIZE 1024
#define RAW 1
#define SCALED 2
#define NUM_MOTORS 4
#define DT 0.02 //integration constant
#define MSZ 3 //matrix size
#define QSZ 4 //quaternion size

/*******************************************************************************
 * VARIABLES                                                                   *
 ******************************************************************************/
mavlink_system_t mavlink_system = {
    1, // System ID (1-255)
    MAV_COMP_ID_AUTOPILOT1 // Component ID (a MAV_COMPONENT value)
};

enum RC_channels {
    THR,
    AIL,
    ELE,
    RUD,
    HASH,
    SWITCH_A,
    SWITCH_B,
    SWITCH_C,
    SWITCH_D,
    SWITCH_E
}; //map of the rc txmitter to the RC_channels

enum motors {
    MOTOR_1,
    MOTOR_2,
    MOTOR_3,
    MOTOR_4
};


const uint16_t RC_raw_fs_scale = RC_RAW_TO_FS;
static int8_t RC_system_online = FALSE;


RCRX_channel_buffer RC_channels[CHANNELS] = {RC_RX_MID_COUNTS};
struct IMU_out IMU_raw = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; //container for raw IMU data
struct IMU_out IMU_scaled = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; //container for scaled IMU data
static uint8_t pub_RC_servo = FALSE;
static uint8_t pub_RC_signals = TRUE;
static uint8_t pub_IMU = FALSE;

/*******************************************************************************
 * TYPEDEFS                                                                    *
 ******************************************************************************/
/* inner loop gyro rate controllers*/

PID_controller pitch_rate_controller = {
    .dt = DT,
    .kp = 120.0,
    .ki = 20.0,
    .kd = 0.0,
    .u_max = 2000.0,
    .u_min = -2000.0
};

PID_controller roll_rate_controller = {
    .dt = DT,
    .kp = 100.0,
    .ki = 20.0,
    .kd = 0.0,
    .u_max = 10000.0,
    .u_min = -10000.0
};

PID_controller roll_controller = {
    .dt = DT,
    .kp = 20.0,
    .ki = 0.0,
    .kd = 0.0,
    .u_max = 1000.0,
    .u_min = -1000.0
};

PID_controller pitch_controller = {
    .dt = DT,
    .kp = 20.0,
    .ki = 0.0,
    .kd = 0.0,
    .u_max = 1000.0,
    .u_min = -1000.0
};

/* container for controller outputs*/
struct controller_outputs {
    float phi;
    float theta;
    float psi;
    float phi_dot;
    float theta_dot;
    float psi_dot;
} controller_outputs;

/*******************************************************************************
 * FUNCTION PROTOTYPES                                                         *
 ******************************************************************************/
/**
 * @function check_IMU_events(void)
 * @param none
 * @brief detects when IMU SPI transaction completes and then publishes data over Mavlink
 * @author Aaron Hunter
 */
void check_IMU_events(void);
/**
 * @function RC_channels_init(void)
 * @param none
 * @brief set all RC channels to RC_RX_MID_COUNTS
 * @author Aaron Hunter
 */
void check_RC_events();
/**
 * @function check_radio_events(void)
 * @param none
 * @brief looks for messages sent over the radio serial port to OSAVC, parses
 * them and provides responses, if needed
 * @note currently only pushing information to usb-serial port
 * @author Aaron Hunter
 */
void check_radio_events(void);

/**
 * @function publish_IMU_data()
 * @param data_type RAW or SCALED
 * @brief reads module level IMU data and publishes over radio serial in Mavlink
 * @author Aaron Hunter
 */
void publish_IMU_data(uint8_t data_type);
/**
 * @function publish_RC_signals_raw(void)
 * @param none
 * @brief scales raw RC signals
 * @author Aaron Hunter
 */
void publish_RC_signals_raw(void);
/**
 * @Function publish_encoder_data()
 * @param none
 * @brief publishes motor data and steering angle data over MAVLink to radio
 * @return none
 * @author Aaron Hunter
 */
void publish_heartbeat(void);

/**
 * @Function publish_parameter(uint8_t param_id[16])
 * @param parameter ID
 * @brief invokes mavlink helper to send out stored parameter 
 * @author aaron hunter
 */
void publish_parameter(uint8_t param_id[16]);

/**
 * @Function calc_pw(uint16_t raw_counts)
 * @param raw counts from the radio transmitter (11 bit unsigned int)
 * @return pulse width in microseconds
 * @brief converts the RC input into the equivalent pulsewidth output for servo
 * and ESC control
 * @author aahunter
 * @modified <Your Name>, <year>.<month>.<day> <hour> <pm/am> */
static int calc_pw(int raw_counts);

/**
 * @Function set_control_output(float gyros[], float euler[])
 * @param none
 * @return none
 * @brief converts RC input signals to pulsewidth values and sets the actuators
 * (servos and ESCs) to those values
 * @author Aaron Hunter
 */
void set_control_output(float gyros[], float euler[]);

/**
 * @Function void calc_angle_rate_output(float gyros[])
 * @param gyros[], the gyro rate measurements
 * @brief computes the output of the gyro rate controllers and stores in 
 * controller_ref struct
 */
void calc_angle_rate_output(float gyros[]);

/**
 * @Function void calc_angle_output(float euler[])
 * @param euler[], the euler angle measurements
 * @brief computes the output of the angle controllers and stores in 
 * controller_ref struct
 */
void calc_angle_output(float euler[]);


/**
 * @Function set_motor_outputs(void);
 * @return none
 * @brief computes the output of the motors
 * @author Aaron Hunter
 */
void set_motor_outputs(void);


/**
 * @Function get_control_output(float ref, float sensor_val, PID_controller * controller)
 * @param ref, the control reference
 * @paarm sensor, sensor measurement
 * @param controller, the PID controller 
 * @return controller output
 * @brief generates controller output
 * @author Aaron Hunter
 */
float get_control_output(float ref, float sensor_val, PID_controller * controller);

/*******************************************************************************
 * FUNCTIONS                                                                   *
 ******************************************************************************/

/**
 * @function check_IMU_events(void)
 * @param none
 * @brief detects when IMU SPI transaction completes and then publishes data over Mavlink
 * @author Aaron Hunter
 */
void check_IMU_events(void) {
    if (IMU_is_data_ready() == TRUE) {
        IMU_get_raw_data(&IMU_raw);
    }
}

/**
 * @function check_RC_events(void)
 * @param none
 * @brief checks for RC messages and stores data in RC channel buffer
 * @author Aaron Hunter
 */
void check_RC_events() {
    if (RCRX_new_cmd_avail()) {
        RCRX_get_cmd(RC_channels);
    }
}

/**
 * @function check_radio_events(void)
 * @param none
 * @brief looks for messages sent over the radio serial port to OSAVC, parses
 * them and provides responses, if needed
 * @note currently only pushing information to usb-serial port
 * @author Aaron Hunter
 */
void check_radio_events(void) {
    uint8_t channel = MAVLINK_COMM_0;
    uint8_t msg_byte;
    uint16_t msg_length;
    uint8_t msg_buffer[BUFFER_SIZE];
    mavlink_message_t msg_rx;
    mavlink_status_t msg_rx_status;

    //MAVLink command structs
    mavlink_heartbeat_t heartbeat;
    mavlink_command_long_t command_qgc;
    mavlink_param_request_read_t param_read;

    if (Radio_data_available()) {
        msg_byte = Radio_get_char();
        if (mavlink_parse_char(channel, msg_byte, &msg_rx, &msg_rx_status)) {
            switch (msg_rx.msgid) {
                case MAVLINK_MSG_ID_HEARTBEAT:
                    mavlink_msg_heartbeat_decode(&msg_rx, &heartbeat);
                    if (heartbeat.type)
                        printf("heartbeat received type(%d)\r\n", heartbeat.type);
                    break;
                case MAVLINK_MSG_ID_COMMAND_LONG:
                    mavlink_msg_command_long_decode(&msg_rx, &command_qgc);
                    printf("Command ID %d received from Ground Control\r\n", command_qgc.command);
                    break;
                case MAVLINK_MSG_ID_PARAM_REQUEST_READ:
                    mavlink_msg_param_request_read_decode(&msg_rx, &param_read);
                    printf("Parameter request ID %s received from Ground Control\r\n", param_read.param_id);
                    publish_parameter(param_read.param_id);
                    break;
                default:
                    printf("Received message with ID %d, sequence: %d from component %d of system %d\r\n",
                            msg_rx.msgid, msg_rx.seq, msg_rx.compid, msg_rx.sysid);
                    break;
            }
        }
    }
}

/**
 * @function publish_IMU_data()
 * @param none
 * @brief reads module level IMU data and publishes over radio serial in Mavlink
 * @author Aaron Hunter
 */
//
void publish_IMU_data(uint8_t data_type) {
    float gyro_x_bias = -0.850931981566821;
    char message[BUFFER_SIZE];
    uint8_t msg_len = 0;
    uint16_t index = 0;
    uint8_t IMU_id = 0;
    if (data_type == RAW) {
        msg_len = sprintf(message, "%f %f %f \r\n", IMU_scaled.gyro.x - gyro_x_bias, IMU_scaled.gyro.y, IMU_scaled.gyro.z);
    } else if (data_type == SCALED) {
            ;
    }
    // msg_length = mavlink_msg_to_send_buffer(msg_buffer, &msg_tx);
    for (index = 0; index < msg_len; index++) {
        Radio_put_char(message[index]);
    }
}

/**
 * @Function publish_encoder_data()
 * @param none
 * @brief publishes motor data and steering angle data over MAVLink using 
 * rc_channels_scaled. This is a bit of a hack until I can generate my own message
 * @return none
 * @author Aaron Hunter
 */

/**
 * @function publish_RC_signals(void)
 * @param none
 * @brief scales raw RC signals into +/- 10000
 * @author Aaron Hunter
 */
void publish_RC_signals(void) {
    mavlink_message_t msg_tx;
    uint16_t msg_length;
    uint8_t msg_buffer[BUFFER_SIZE];
    uint16_t index = 0;
    uint8_t RC_port = 0; //first 8 channels 
    int16_t scaled_channels[CHANNELS];
    uint8_t rssi = 255; //unknown--may be able to extract from receiver
    for (index = 0; index < CHANNELS; index++) {
        scaled_channels[index] = (RC_channels[index] - RC_RX_MID_COUNTS) * RC_raw_fs_scale;
    }
    mavlink_msg_rc_channels_scaled_pack(mavlink_system.sysid,
            mavlink_system.compid,
            &msg_tx,
            Sys_timer_get_msec(),
            RC_port,
            scaled_channels[0],
            scaled_channels[1],
            scaled_channels[2],
            scaled_channels[3],
            scaled_channels[4],
            scaled_channels[5],
            scaled_channels[6],
            scaled_channels[7],
            rssi);
    msg_length = mavlink_msg_to_send_buffer(msg_buffer, &msg_tx);
    for (index = 0; index < msg_length; index++) {
        Radio_put_char(msg_buffer[index]);
    }
}

/**
 * @function publish_RC_signals_raw(void)
 * @param none
 * @brief scales raw RC signals
 * @author Aaron Hunter
 */
//void publish_RC_signals_raw(void) {
//    mavlink_message_t msg_tx;
//    uint16_t msg_length;
//    uint8_t msg_buffer[BUFFER_SIZE];
//    uint16_t index = 0;
//    uint8_t RC_port = 0; //first 8 channels 
//    uint8_t rssi = 255; //unknown--may be able to extract from receiver
//    mavlink_msg_rc_channels_raw_pack(mavlink_system.sysid,
//            mavlink_system.compid,
//            &msg_tx,
//            Sys_timer_get_msec(),
//            RC_port,
//            RC_channels[0],
//            RC_channels[1],
//            RC_channels[2],
//            RC_channels[3],
//            RC_channels[4],
//            RC_channels[5],
//            RC_channels[6],
//            RC_channels[7],
//            rssi);
//    msg_length = mavlink_msg_to_send_buffer(msg_buffer, &msg_tx);
//    for (index = 0; index < msg_length; index++) {
//        Radio_put_char(msg_buffer[index]);
//    }
//}

/**
 * @function publish_RC_signals_raw(void)
 * @param none
 * @brief scales raw RC signals
 * @author Aaron Hunter
 */
void publish_RC_signals_raw(void) {
    int index = 0;
    uint16_t msg_length; 
    char msg_buffer[BUFFER_SIZE];
    msg_length = sprintf(msg_buffer, "%d %d %d %d %d %d %d %d %d %d\r\n", 
        RC_channels[0],
        RC_channels[1],
        RC_channels[2],
        RC_channels[3],
        RC_channels[4],
        RC_channels[5],
        RC_channels[6], 
        RC_channels[7],        
        RC_channels[8], 
        RC_channels[9]);
    for (index = 0; index < msg_length; index++) {
        Radio_put_char(msg_buffer[index]);
    }
}



/**
 * @Function publish_heartbeat(void)
 * @param none
 * @brief invokes mavlink helper to generate heartbeat and sends out via the radio
 * @author aaron hunter
 */
void publish_heartbeat(void) {
    mavlink_message_t msg_tx;
    uint16_t msg_length;
    uint8_t msg_buffer[BUFFER_SIZE];
    uint16_t index = 0;
    uint8_t mode = MAV_MODE_FLAG_MANUAL_INPUT_ENABLED | MAV_MODE_FLAG_SAFETY_ARMED;
    uint32_t custom = 0;
    uint8_t state = MAV_STATE_STANDBY;
    mavlink_msg_heartbeat_pack(mavlink_system.sysid
            , mavlink_system.compid,
            &msg_tx,
            MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_GENERIC,
            mode,
            custom,
            state);
    msg_length = mavlink_msg_to_send_buffer(msg_buffer, &msg_tx);
    for (index = 0; index < msg_length; index++) {
        Radio_put_char(msg_buffer[index]);
    }
}

/**
 * @Function publish_parameter(uint8_t param_id[16])
 * @param parameter ID
 * @brief invokes mavlink helper to send out stored parameter 
 * @author aaron hunter
 */
void publish_parameter(uint8_t param_id[16]) {
    mavlink_message_t msg_tx;
    uint16_t msg_length;
    uint8_t msg_buffer[BUFFER_SIZE];
    uint16_t index = 0;
    float param_value = 320.0; // value of the requested parameter
    uint8_t param_type = MAV_PARAM_TYPE_INT16; // onboard mavlink parameter type
    uint16_t param_count = 1; // total number of onboard parameters
    uint16_t param_index = 1; //index of this value
    mavlink_msg_param_value_pack(mavlink_system.sysid,
            mavlink_system.compid,
            &msg_tx,
            param_id,
            param_value,
            param_type,
            param_count,
            param_index
            );
    msg_length = mavlink_msg_to_send_buffer(msg_buffer, &msg_tx);
    for (index = 0; index < msg_length; index++) {
        Radio_put_char(msg_buffer[index]);
    }
}

/**
 * @Function calc_pw(uint16_t raw_counts)
 * @param raw counts from the radio transmitter (11 bit unsigned int)
 * @return pulse width in microseconds
 * @brief converts the RC input into the equivalent pulsewidth output for servo
 * and ESC control
 * @author aahunter
 * @modified <Your Name>, <year>.<month>.<day> <hour> <pm/am> */
static int calc_pw(int raw_counts) {
    const int denominator = (RC_RX_MAX_COUNTS - RC_RX_MIN_COUNTS);
    const int numerator = (RC_SERVO_MAX_PULSE - RC_SERVO_MIN_PULSE);
    int pulse_width; //servo output in microseconds

    pulse_width = (raw_counts - RC_RX_MID_COUNTS) * numerator; // scale to servo values
    pulse_width = pulse_width / denominator; // divide out counts
    pulse_width = pulse_width + RC_SERVO_CENTER_PULSE; // add in midpoint of servo pulse
    return pulse_width;
}

/**
 * @Function void calc_angle_rate_output(float gyros[])
 * @param gyros[], the gyro rate measurements
 * @brief computes the output of the gyro rate controllers and stores in 
 * controller_ref struct
 */
void calc_angle_rate_output(float gyros[]) {
    float roll_rate_cmd;
    float pitch_rate_cmd;
    roll_rate_cmd = get_control_output(controller_outputs.phi, gyros[0], &roll_rate_controller);
    pitch_rate_cmd = get_control_output(controller_outputs.theta, gyros[1], &pitch_rate_controller);
    controller_outputs.phi_dot = roll_rate_cmd;
    controller_outputs.theta_dot = pitch_rate_cmd;
}

/**
 * @Function void calc_angle_output(float euler[])
 * @param euler[], the euler angle measurements
 * @brief computes the output of the angle controllers and stores in 
 * controller_ref struct
 */
void calc_angle_output(float euler[]){
    float roll_cmd;
    float pitch_cmd;
    /* NOTE: Euler angles are defined a yaw, pitch, roll for some stupid reason*/
    roll_cmd = get_control_output(0.0, euler[2], &roll_controller);
    pitch_cmd = get_control_output(0.0, euler[1], &pitch_controller);
    controller_outputs.phi = roll_cmd;
    controller_outputs.theta = pitch_cmd;
}

/**
 * @Function set_control_output(float gyros[], float euler[])
 * @param none
 * @return none
 * @brief converts RC input signals to pulsewidth values and sets the actuators
 * (servos and ESCs) to those values
 * @author Aaron Hunter
 */
void set_control_output(float gyros[], float euler[]) {
    float gyro_x_bias = -0.850931981566821;
    int hash;
    int switch_d;
    int throttle[4];
    int throttle_raw;
    float roll_cmd;
    float pitch_cmd;
    int roll_rate_cmd;
    int pitch_rate_cmd;
    int yaw_cmd;
    int hash_check;
    const int tol = 4;
    int INTOL;

    int phi_raw;
    int theta_raw;
    int psi_raw;
    /* get RC commanded values*/
    switch_d = RC_channels[SWITCH_D];
    throttle_raw = RC_channels[THR];
    phi_raw = RC_channels[AIL];
    theta_raw = RC_channels[ELE];
    psi_raw = RC_channels[RUD];
    //    psi_raw = 0; 
    hash = RC_channels[HASH];
    hash_check = (throttle_raw >> 2) + (phi_raw >> 2) + (theta_raw >> 2) + (psi_raw >> 2);
    if (abs(hash_check - hash) <= tol) {
        INTOL = TRUE;
        /*compute attitude commands*/
        roll_rate_cmd = (int) get_control_output(0.0, gyros[0]- gyro_x_bias, &roll_rate_controller);
        pitch_rate_cmd = (int) get_control_output(0.0, gyros[1], &pitch_rate_controller);
        yaw_cmd = -(psi_raw - RC_RX_MID_COUNTS) >> 2; // reverse for CCW positive yaw
        if(RC_channels[SWITCH_D] == RC_RX_MAX_COUNTS || RC_channels[SWITCH_D] == RC_RX_MIN_COUNTS) {
            if (RC_channels[SWITCH_D] == RC_RX_MAX_COUNTS) { // SWITCH_D arms the motors
                /* mix attitude into X configuration */
                throttle[0] = calc_pw(throttle_raw + roll_rate_cmd);
                throttle[1] = calc_pw(throttle_raw - roll_rate_cmd);
                //            throttle[2] = calc_pw(throttle_raw - roll_rate_cmd + pitch_rate_cmd - yaw_cmd);
                //            throttle[3] = calc_pw(throttle_raw + roll_rate_cmd + pitch_rate_cmd + yaw_cmd);

            } else { // Set throttle to minimum
                throttle[0] = RC_SERVO_MIN_PULSE;
                throttle[1] = RC_SERVO_MIN_PULSE;
                throttle[2] = RC_SERVO_MIN_PULSE;
                throttle[3] = RC_SERVO_MIN_PULSE;
            }
        }
        /* send commands to motor outputs*/
        RC_servo_set_pulse(throttle[0], MOTOR_1);
        RC_servo_set_pulse(throttle[1], MOTOR_2);
//        RC_servo_set_pulse(throttle[2], MOTOR_3);
//        RC_servo_set_pulse(throttle[3], MOTOR_4);
    } else {
        INTOL = FALSE;
        
        //        printf("%d, %d, %d, %d, %d, %d, %d, %d \r\n", switch_d, throttle_raw, phi_raw, theta_raw, psi_raw, hash, hash_check, INTOL);
    }
}

/**
 * @Function set_motor_outputs(float theta_dot, float phi_dot);
 * @param theta_dot, pitch rate
 * @param phi_dot, roll rate
 * @return none
 * @brief converts RC input signals and controller outputs to pulsewidth values and sets the actuators
 * (servos and ESCs) to those values
 * @author Aaron Hunter
 */
void set_motor_outputs(void) {
    char message[BUFFER_SIZE];
    uint8_t msg_len = 0;
    int hash;
    int switch_d;
    int throttle[4];
    int thr_bias[4];
    thr_bias[MOTOR_1] = 610- RC_RX_MIN_COUNTS;
    thr_bias[MOTOR_2] = 440 - RC_RX_MIN_COUNTS;
    int throttle_raw;
    int yaw_cmd;
    int hash_check;
    const int tol = 4;
    int INTOL;
    int phi_raw;
    int theta_raw;
    int psi_raw;
    /* get RC commanded values*/
    switch_d = RC_channels[SWITCH_D];
    throttle_raw = RC_channels[THR];
    phi_raw = RC_channels[AIL];
    theta_raw = RC_channels[ELE];
    psi_raw = RC_channels[RUD];
    hash = RC_channels[HASH];
    hash_check = (throttle_raw >> 2) + (phi_raw >> 2) + (theta_raw >> 2) + (psi_raw >> 2);
    if (abs(hash_check - hash) <= tol) {
        INTOL = TRUE;
        yaw_cmd = -(psi_raw - RC_RX_MID_COUNTS) >> 2; // reverse for CCW positive yaw
        if (RC_channels[SWITCH_D] == RC_RX_MAX_COUNTS) { // SWITCH_D arms the motors
            /* mix attitude into X configuration */
            throttle[0] = calc_pw(throttle_raw + thr_bias[MOTOR_1]+ controller_outputs.phi_dot);
            throttle[1] = calc_pw(throttle_raw + thr_bias[MOTOR_2] - controller_outputs.phi_dot );
            throttle[2] = calc_pw(throttle_raw - controller_outputs.phi_dot + controller_outputs.theta_dot - yaw_cmd);
            throttle[3] = calc_pw(throttle_raw + controller_outputs.phi_dot + controller_outputs.theta_dot + yaw_cmd);

        } else { // Set throttle to minimum
            throttle[0] = RC_SERVO_MIN_PULSE;
            throttle[1] = RC_SERVO_MIN_PULSE;
            throttle[2] = RC_SERVO_MIN_PULSE;
            throttle[3] = RC_SERVO_MIN_PULSE;
        }
        /* send commands to motor outputs*/
        RC_servo_set_pulse(throttle[0], MOTOR_1);
        RC_servo_set_pulse(throttle[1], MOTOR_2);
        RC_servo_set_pulse(throttle[2], MOTOR_3);
        RC_servo_set_pulse(throttle[3], MOTOR_4);
    } else {
        INTOL = FALSE;
        sprintf(message, "%d, %d, %d, %d, %d, %d, %d, %d \r\n", switch_d, throttle_raw, phi_raw, theta_raw, psi_raw, hash, hash_check, INTOL);
    }
}

float get_control_output(float ref, float sensor_val, PID_controller * controller) {
    float setpoint = 0;
    /* get control from PID*/
    PID_update(controller, ref, sensor_val);
    setpoint = controller->u;
    return (setpoint);
}

int main(void) {
    uint32_t start_time = 0;
    uint32_t cur_time = 0;
    uint32_t RC_timeout = 1000;
    uint32_t angular_rate_control_start_time = 0;
    uint32_t angle_control_start_time = 0;
    uint32_t heartbeat_start_time = 0;
    uint8_t index;
    int8_t IMU_state = ERROR;
    int8_t IMU_retry = 5;
    uint32_t IMU_error = 0;
    uint8_t error_report = 50;
    uint32_t IMU_update_start;
    uint32_t IMU_update_end;

    /*test value for IMU update rate*/
    int8_t IMU_updated = TRUE;

    /*radio variables*/
    char message[BUFFER_SIZE];
    uint8_t msg_len = 0;

    /*filter gains*/
    float kp_a = 2.5; //accelerometer proportional gain
    float ki_a = 0.05; // accelerometer integral gain
    float kp_m = 2.5; // magnetometer proportional gain
    float ki_m = 0.05; //magnetometer integral gain
    /*timing and conversion*/
    const float dt = DT;
    const float deg2rad = M_PI / 180.0;
    const float rad2deg = 180.0 / M_PI;
    /* Calibration matrices and offset vectors */

    /*calibration matrices*/
    float A_acc[MSZ][MSZ] = {
        {5.98605657636023e-05, 5.02299172664344e-08, 8.41134559461075e-07},
        {-2.82167981801537e-08, 6.05938345982234e-05, 6.95665927111956e-07},
        {4.48326742757725e-08, -3.34771681800715e-07, 5.94633160681115e-05}
    };
    float A_mag[MSZ][MSZ] = {
        {0.00333834334834959, 2.58649731866218e-05, -4.47182534891735e-05},
        {3.97521279910819e-05, 0.00341838979684877, -7.55578863505947e-06},
        {-6.49436573527762e-05, 3.05050635014235e-05, 0.00334143925188739}
    };
    float b_acc[MSZ] = {0.00591423067694908, 0.0173747801090554, 0.0379428158730668};
    float b_mag[MSZ] = {0.214140746707571, -1.08116057610690, -0.727337561140470};
    // gravity inertial vector
    float a_i[MSZ] = {0, 0, 1.0};
    // Earth's magnetic field inertial vector, normalized 
    // North 22,680.8 nT	East 5,217.6 nT	Down 41,324.7 nT, value from NOAA
    // converted into ENU format and normalized:
    float m_i[MSZ] = {0.110011998753301, 0.478219898291142, -0.871322609031072};
    
    /*attitude*/
    float q_test[QSZ] = {1, 0, 0, 0};
    /*gyro bias*/
    float bias_test[MSZ] = {0, 0, 0};
    /*euler angles (yaw, pitch, roll) */

    // Euler angles
    float euler[MSZ] = {0, 0, 0};

    /* data arrays */
    float gyro_cal[MSZ] = {0, 0, 0};
    float acc_cal[MSZ] = {0, 0, 0};
    float mag_cal[MSZ] = {0, 0, 0};

    //Initialization routines
    Board_init(); //board configuration
    Serial_init(); //start debug terminal 
    Radio_serial_init(); //start the radios
    printf("Board initialization complete.\r\n");
    msg_len = sprintf(message, "Board initialization complete.\r\n");
    for (index = 0; index < msg_len; index++) {
        Radio_put_char(message[index]);
    }

    Sys_timer_init(); //start the system timer
    cur_time = Sys_timer_get_msec();
    printf("System timer initialized.  Current time %d. \r\n", cur_time);
    msg_len = sprintf(message, "System timer initialized.\r\n");
    for (index = 0; index < msg_len; index++) {
        Radio_put_char(message[index]);
    }
    cur_time = Sys_timer_get_msec();
    start_time = cur_time;
    RCRX_init(); //initialize the radio control system
    /*wait until we get data from the RC controller*/
    while (cur_time - start_time < RC_timeout) {
        if (RCRX_new_cmd_avail()) {
            RC_system_online = TRUE;
            break;
        }
    }
    if (RC_system_online == FALSE) {
        msg_len = sprintf(message, "RC system failed to connect!\r\n");
    } else {
        msg_len = sprintf(message, "RC system online.\r\n");
    }
    for (index = 0; index < msg_len; index++) {
        Radio_put_char(message[index]);
    }

    /* With RC controller online we can set the servo PWM outputs*/
    RC_servo_init(ESC_UNIDIRECTIONAL_TYPE, SERVO_PWM_1); // MOTOR 1
    RC_servo_init(ESC_UNIDIRECTIONAL_TYPE, SERVO_PWM_2); // MOTOR 2
    RC_servo_init(ESC_UNIDIRECTIONAL_TYPE, SERVO_PWM_3); // MOTOR 3
    RC_servo_init(ESC_UNIDIRECTIONAL_TYPE, SERVO_PWM_4); // MOTOR 4
    /* initialize the IMU */
    IMU_state = IMU_init(IMU_SPI_MODE);
    if (IMU_state == ERROR && IMU_retry > 0) {
        IMU_state = IMU_init(IMU_SPI_MODE);
        printf("IMU failed init, retrying %d \r\n", IMU_retry);
        IMU_retry--;
    }
    /*initialize controllers*/
    PID_init(&pitch_rate_controller);
    PID_init(&roll_rate_controller);
    PID_init(&pitch_controller);
    PID_init(&roll_controller);

    printf("\r\nQuad Passthrough Control App %s, %s \r\n", __DATE__, __TIME__);
    printf("Testing!\r\n");
    /* load IMU calibrations */
    IMU_set_mag_cal(A_mag, b_mag);
    IMU_set_acc_cal(A_acc, b_acc);

    /* set filter gains and inertial guiding vectors for AHRS*/
    AHRS_set_filter_gains(kp_a, ki_a, kp_m, ki_m);
    AHRS_set_mag_inertial(m_i);

    cur_time = Sys_timer_get_msec();
    angular_rate_control_start_time = cur_time;
    angle_control_start_time = cur_time;
    heartbeat_start_time = cur_time;

    while (1) {
        //check for all events
        check_IMU_events(); //check for IMU data ready and publish when available
        //        check_radio_events(); //detect and process MAVLink incoming messages
        check_RC_events(); //check incoming RC commands
        cur_time = Sys_timer_get_msec();
        //publish control and sensor signals
        if (cur_time - angular_rate_control_start_time >= ANGULAR_RATE_CONTROL_PERIOD) {
            angular_rate_control_start_time = cur_time; //reset control loop timer
//            set_control_output(gyro_cal, euler); // set actuator outputs
            calc_angle_rate_output(gyro_cal);
            set_motor_outputs();
            /*start next data acquisition round*/
            IMU_state = IMU_start_data_acq(); //initiate IMU measurement with SPI
            if (IMU_updated == TRUE) {
                IMU_update_start = Sys_timer_get_msec();
                IMU_updated = FALSE;
            }
            if (IMU_state == ERROR) {
                IMU_error++;
                if (IMU_error % error_report == 0) {
                    printf("IMU error count %d\r\n", IMU_error);
                    //                    IMU_retry = 5;
                    //                    IMU_state = IMU_init(IMU_SPI_MODE);
                    //                    if (IMU_state == ERROR && IMU_retry > 0) {
                    //                        IMU_state = IMU_init(IMU_SPI_MODE);
                    //                        printf("IMU failed init, retrying %d \r\n", IMU_retry);
                    //                        IMU_retry--;
                }
            }
            /*publish high speed sensors*/
            if (pub_RC_signals == TRUE) {
                publish_RC_signals_raw();
            }
            if (pub_IMU == TRUE) {
                publish_IMU_data(RAW);
            }
        }
        /* update angular control every ANGL_CONTROL_PERIOD*/
//        if(cur_time - angle_control_start_time >= ANGLE_CONTROL_PERIOD) {
//            angle_control_start_time = cur_time;
//            calc_angle_output(euler);
//        }
        
        if (IMU_is_data_ready() == TRUE) {
            IMU_updated = TRUE;
            IMU_update_end = Sys_timer_get_msec();
            IMU_get_norm_data(&IMU_scaled);

            acc_cal[0] = (float) IMU_scaled.acc.x;
            acc_cal[1] = (float) IMU_scaled.acc.y;
            acc_cal[2] = (float) IMU_scaled.acc.z;
            mag_cal[0] = (float) IMU_scaled.mag.x;
            mag_cal[1] = (float) IMU_scaled.mag.y;
            mag_cal[2] = (float) IMU_scaled.mag.z;
            /*scale gyro readings into rad/sec */
            gyro_cal[0] = (float) IMU_scaled.gyro.x * deg2rad;
            gyro_cal[1] = (float) IMU_scaled.gyro.y * deg2rad;
            gyro_cal[2] = (float) IMU_scaled.gyro.z * deg2rad;
            AHRS_update(acc_cal, mag_cal, gyro_cal, dt, q_test, bias_test);

//             printf("%+3.1f, %+3.1f, %+3.1f, %d \r\n", euler[0] * rad2deg, euler[1] * rad2deg, euler[2] * rad2deg, IMU_update_end - IMU_update_start);
        }
        /* if period timer expires, publish the heartbeat message*/
        if (cur_time - heartbeat_start_time >= HEARTBEAT_PERIOD) {
            heartbeat_start_time = cur_time; //reset the timer
            //            publish_heartbeat();
        }
    }
    return (0);
}
