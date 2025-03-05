// snd_analizer.h
#ifndef SND_ANALIZER_H
#define SND_ANALIZER_H

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "i2c-display-lib.h"

// Константи
#define ADC_PIN 26             // Використовуємо GPIO 26 для АЦП
#define MEASURE_PIN 21          // Кнопка підключена до GPIO 21
#define SAMPLE_ARRAY_SIZE 4000 // Кількість зразків для зберігання
#define GRAPH_LENGTH 8
#define GRAPH_SLICE_LENGTH 5
#define TOTAL_SLICES (GRAPH_LENGTH * GRAPH_SLICE_LENGTH) // Загальна кількість слайсів
#define SAMPLE_INTERVAL_MS 1   // Інтервал вибірки, 1 мс
#define ADC_NOISE_THRESHOLD 50 // Мінімальне відхилення від шуму
#define BUTTON_DEBOUNCE_US 100000 // 100 мс
#define LCD_SDA_PIN 16
#define LCD_SCL_PIN 17
#define ENCODER_DT_PIN 4       // DT енкодера на GPIO 4
#define ENCODER_CLK_PIN 5      // CLK енкодера на GPIO 5
#define POINTER_POSITION 7     // 7 = нижній піксель, 0 = верхній піксель

#define SAMPLE_INTERVAL_MS 1 // 1 мс = 1000 Гц
#define MIN_PEAK_DURATION 10 // 0.01 с = 10 записів при 1000 Гц
#define NEXT_PEAK_PIN 3 // GPIO3 для навігації по максимумах

// Статичні константи
extern const uint16_t ADC_NOISE;
extern const int SAMPLE_SLICE;
extern const float CONVERSION_FACTOR;
extern uint32_t saved_slices_maximums[TOTAL_SLICES];

// Глобальні змінні
extern uint16_t adc_values[SAMPLE_ARRAY_SIZE];
extern int sample_index;
extern bool collecting_data;
extern bool data_collection_complete;
extern struct repeating_timer timer;
extern uint32_t saved_slices_averages[TOTAL_SLICES];
extern int encoder_slice_index;
extern bool encoder_active;
extern bool encoder_update_needed;
extern uint8_t lcd_segment[8];
extern int peak_slices[TOTAL_SLICES];
extern int peak_count;
extern int current_peak_index;

// Прототипи функцій
void timer_start(void);
void timer_stop(void);
void lcd_set_cursor(int, int);
void draw_graph_on_lcd(void);
void init_encoder(void);
void gpio_interrupt_handler(uint gpio, uint32_t events);
int scale_adc_value(uint32_t average);
int is_noise(uint32_t value);
bool repeating_timer_callback(struct repeating_timer *t);
void measure_pin_pressed(void);
void measure_pin_released(void);
void clear_adc_array(void);
void print_data(void);
void init_adc(void);
void measure_pin_init(void);
bool debounce_check(uint64_t current_time, uint64_t* last_event_time, uint32_t debounce_us);
bool is_encoder_event(uint gpio);
bool is_encoder_rotation_right(bool clk_state, bool dt_state);
void handle_encoder_rotation(uint32_t events);
bool is_measure_pin_event(uint gpio);
void handle_measure_pin_event(uint64_t current_time, uint64_t* last_event_time, uint32_t events);
void init_system(void);
void lcd_segment_clear(void);
void handle_pointer_pixel(int bit_position, int value);
void set_lcd_segment_row(int pos, int value, bool disable_bottom);
void lcd_segment_write(int cursor_position);
uint32_t calculate_average(int from, int to);
void print_slices_averages(uint32_t slices_averages[], int slices_count);
void display_slice_info(int sample_count, int slice_length);
void calculate_slice_averages(int effective_samples, int slice_length, 
                              uint32_t* slices_averages, uint32_t* saved_slices_averages);
void display_graph(uint32_t* slices_averages);
void lcd_hello(void);
bool should_update_encoder_display(void);
void update_encoder_display(void);
void init_next_peak_pin();
void move_to_next_peak();
int calculate_peak_duration(int slice_start, int slice_end);
void analyze_peaks(int slice_length);
void display_peak_info();
float adc_to_volt(uint16_t adc_value);

#endif // SND_ANALIZER_H
