#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/list.h"
#include "ESP32Servo.h"
#include "arm.h"
#include "rover_config.h"
#include "esp_timer.h"
#include "esp_err.h"

#define ARM_UPDATE_INTERVAL_MS  10

typedef enum ArmDirection {
  DIRECTION_POSITIVE,
  DIRECTION_NEGATIVE
} ArmDirection;

typedef struct PathPoint {
  xListItem   item;
  uint16_t    pos;
  uint16_t    delay_to_next;
} PathPoint;

typedef struct ArmServo {
  Servo*        servo;
  uint16_t      min;
  uint16_t      max;
  uint16_t      current_pos;
  uint16_t      start_pos;
  uint16_t      end_pos;
  uint8_t       speed;
  uint32_t      num_moves;
  uint32_t      steps_per_move;
  uint32_t      num_moves_finished;
  ArmDirection  direction;
  bool          isPaused;
  xList         path;
} ArmServo;

static void update_arm_positions_timer_cb(void* arg);

static ArmServo armServos[ARM_NUM_AXIS];
static esp_timer_handle_t arm_update_timer;
static xSemaphoreHandle update_axis_sem_handle;

void arm_init(Servo* servos, uint8_t num_servos)
{
  memset(&armServos, 0, sizeof(armServos));
  
  update_axis_sem_handle = xSemaphoreCreateBinary();
  assert(update_axis_sem_handle != NULL);
  xSemaphoreGive(update_axis_sem_handle);
  
  for (uint8_t i = 0; i < num_servos; i++) {
    vListInitialise((xList*)&armServos[i].path);
    armServos[i].current_pos = 1500;
    armServos[i].servo = &servos[i];
  }

  esp_timer_create_args_t periodic_timer_args;
  periodic_timer_args.callback = &update_arm_positions_timer_cb;
  periodic_timer_args.name = "update_arm_servo";

  ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &arm_update_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(arm_update_timer, ARM_UPDATE_INTERVAL_MS * 1000));
}

void arm_move_axis_us(ArmAxis axisNum, uint16_t pos, uint8_t speed)
{
  ArmServo* axis = &armServos[axisNum];
  uint32_t steps_to_move;
  uint32_t internal_speed;
  assert(pos >= DEFAULT_uS_LOW);
  assert(pos <= DEFAULT_uS_HIGH);
  assert(speed >= ARM_MIN_SPEED);
  assert(speed <= ARM_MAX_SPEED);
  internal_speed = ARM_MAX_SPEED + 1 - speed; // Speed is inverted internally 1 fastest, 10 slowest

  if (pos == axis->current_pos || (pos == axis->end_pos && internal_speed == axis->speed)) return;
  xSemaphoreTake(update_axis_sem_handle, portMAX_DELAY);
  axis->isPaused = false;
  axis->end_pos = pos;
  axis->start_pos = axis->current_pos;
  axis->speed = internal_speed;
  steps_to_move = abs(axis->end_pos - axis->current_pos);
  axis->num_moves = (steps_to_move / ARM_UPDATE_INTERVAL_MS);
  if (axis->num_moves == 0) {
    axis->num_moves = 1;
  }
  axis->steps_per_move = steps_to_move / axis->num_moves;
  axis->num_moves_finished = 0;
  axis->direction = (axis->end_pos > axis->current_pos) ? DIRECTION_POSITIVE : DIRECTION_NEGATIVE;
  xSemaphoreGive(update_axis_sem_handle);
}

void arm_move(ArmAxis axisNum, uint16_t x, uint16_t y, uint16_t z)
{
  // TODO math
}

void arm_pause(ArmAxis axisNum)
{
  armServos[axisNum].isPaused = true;
}

void arm_resume(ArmAxis axisNum)
{
  armServos[axisNum].isPaused = false;
}

static void update_arm_positions_timer_cb(void* arg)
{
  ArmServo* axis;
  uint32_t next_servo_pos;
  for (uint8_t i = 0; i < ARM_NUM_AXIS; i++) {
    axis = &armServos[i];
    if (axis->num_moves == 0) continue;
    if (axis->isPaused) continue;
    
    xSemaphoreTake(update_axis_sem_handle, portMAX_DELAY);
    axis->num_moves_finished++;
    if (axis->num_moves_finished % axis->speed == 0) {
      // Check if last move
      if (abs(axis->current_pos - axis->end_pos) <= axis->steps_per_move) {
        axis->current_pos = axis->end_pos;
        next_servo_pos = axis->end_pos;
        axis->num_moves = 0;
        axis->num_moves_finished = 0;
        axis->start_pos = axis->current_pos;
      } else if (axis->direction == DIRECTION_POSITIVE) {
        next_servo_pos = axis->current_pos + axis->steps_per_move;
        axis->current_pos = axis->current_pos + axis->steps_per_move;
      } else {
        next_servo_pos = axis->current_pos - axis->steps_per_move;
        axis->current_pos = axis->current_pos - axis->steps_per_move;
      }
      xSemaphoreGive(update_axis_sem_handle);
      axis->servo->writeMicroseconds(next_servo_pos);
    } else {
      xSemaphoreGive(update_axis_sem_handle);
    }
  }
}