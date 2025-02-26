#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#include "i2c-display-lib.h"

#define ADC_PIN 26             // Використовуємо GPIO 26 для АЦП
#define BUTTON_PIN 3          // Кнопка підключена до GPIO 21
#define SAMPLE_ARRAY_SIZE 4000 // Кількість зразків для зберігання
#define GRAPH_LENGTH 8
#define GRAPH_SLICE_LENGTH 5
#define SAMPLE_INTERVAL_MS 1   // Інтервал вибірки, 1 мс можна змінити за потреби
#define ADC_NOISE_THRESHOLD 50 // Мінімальне відхилення від шуму
#define BUTTON_DEBOUNCE_US 100000 // 100 мс

#define LCD_SDA_PIN 16
#define LCD_SCL_PIN 17


#define ENCODER_DT_PIN 4            // DT енкодера на GPIO 4
#define ENCODER_CLK_PIN 5           // CLK енкодера на GPIO 5

const uint16_t ADC_NOISE = 2080;
const int SAMPLE_SLICE = SAMPLE_ARRAY_SIZE / GRAPH_LENGTH;
const float CONVERSION_FACTOR = 3.27f / (1 << 12);// Конвертаційний коефіцієнт для перетворення значення АЦП в вольти


void timer_start();
void stop_timer();
void lcd_set_cursor(int, int);
void draw_graph_on_lcd();
void init_encoder();
void gpio_interrupt_handler(uint gpio, uint32_t events);
int scale_adc_value(uint32_t average);
int is_noise(uint32_t value);

uint16_t adc_values[SAMPLE_ARRAY_SIZE];            // Масив для зберігання зчитаних значень

int sample_index = 0; // Індекс для запису в масив
bool collecting_data = false; // Флаг для контролю збирання даних
bool data_collection_complete = false; // Флаг для позначення завершення збирання даних
struct repeating_timer timer; // Глобальна змінна для таймера

uint32_t saved_slices_averages[40]; // Глобальний масив для збереження slices_averages
int encoder_slice_index = 0;        // Поточний індекс для енкодера
bool encoder_active = false;        // Флаг активації енкодера

uint8_t lcd_segment[8] = {
                  0b00000,
                  0b00000,
                  0b00000,
                  0b00000,
                  0b00000,
                  0b00000,
                  0b00000,
                  0b00000
};

// Функція-обробник переривань для таймера
bool repeating_timer_callback(struct repeating_timer *t) {
if (!collecting_data) {
        cancel_repeating_timer(t);
        return false; // Якщо не збираємо дані, не виконуємо функцію
    }

    if (sample_index < SAMPLE_ARRAY_SIZE) { // Зчитування значення з АЦП
        adc_values[sample_index++] = adc_read();
    } else {
        collecting_data = false;
        data_collection_complete = true;
        cancel_repeating_timer(t);
    }
    return true; // Продовжуємо таймер (він буде зупинений в основному циклі)
}

// Обробник переривання для кнопки
void button_start_pressed() {
    if (!collecting_data) {
        timer_start();
    } else {
        printf("Timer already running, ignoring press\n");
    }
}


// Обробник для відпускання кнопки
void button_released() {
    if (collecting_data) {
        collecting_data = false;
        data_collection_complete = true;
        stop_timer();
        printf("Button released, timer stopped\n");
    } else {
        printf("No data collection to stop\n");
    }
}


void clear_adc_array() {
    for (int i = 0; i < SAMPLE_ARRAY_SIZE; i++) {
        adc_values[i] = 0;
    }
}

// Функція для запуску таймера
void timer_start() {
  collecting_data = true;
  sample_index = 0; // Скидаємо індекс
  clear_adc_array();
  if (!add_repeating_timer_ms(-SAMPLE_INTERVAL_MS, repeating_timer_callback, NULL, &timer)) {
    printf("Failed to start timer!\n");
  } else
    printf("Data collection started.\n");
}

// Функція для зупинки таймера
void stop_timer() {
  cancel_repeating_timer(&timer);
  collecting_data = false;
  // sample_index = 0; // Опціонально, якщо хочете скинути індекс
  printf("Timer stopped.\n");
}


// Функція для виводу даних
void print_data() {
  printf("Data collection complete. Samples filled:\n");
  /* for(int i = 0; i < SAMPLE_ARRAY_SIZE; i++) { */
  /*   if (is_noise(adc_values[i])) continue; */
  /*   float voltage = adc_values[i] * CONVERSION_FACTOR; */
  /*   printf("Voltage at moment %d: %f %u\n", i, voltage, adc_values[i]); */
  /* } */
  draw_graph_on_lcd();
  data_collection_complete = false; // Скидаємо флаг для наступного циклу збору даних
}


// Функція для ініціалізації АЦП
void init_adc() {
  adc_init();
  adc_gpio_init(ADC_PIN);
  adc_select_input(0); // Вибираємо ADC0, який відповідає GPIO 26
}


void init_button() {
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    gpio_set_irq_enabled_with_callback(BUTTON_PIN, 
                                       GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, 
                                       true, 
                                       &gpio_interrupt_handler);
}


void init_encoder() {
    gpio_init(ENCODER_DT_PIN);
    gpio_set_dir(ENCODER_DT_PIN, GPIO_IN);
    gpio_pull_up(ENCODER_DT_PIN);
    gpio_init(ENCODER_CLK_PIN);
    gpio_set_dir(ENCODER_CLK_PIN, GPIO_IN);
    gpio_pull_up(ENCODER_CLK_PIN);
    gpio_set_irq_enabled(ENCODER_CLK_PIN, 
                         GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, 
                         true); // Не перезаписуємо callback, лише вмикаємо переривання
}


void gpio_interrupt_handler(uint gpio, uint32_t events) {
    static uint64_t last_button_event_time = 0; // Час останньої події для кнопки
    uint64_t current_time = time_us_64();

    // Обробка кнопки з антидребезгом
    if (gpio == BUTTON_PIN) {
        if (current_time - last_button_event_time < BUTTON_DEBOUNCE_US) {
            printf("Button debounce rejected: %u\n", (uint32_t)(current_time - last_button_event_time));
            return;
        }
        last_button_event_time = current_time;

        printf("Event: %s, Time diff: %u\n", 
               (events & GPIO_IRQ_EDGE_FALL) ? "FALL" : "RISE", 
               (uint32_t)(current_time - last_button_event_time));
        if (events & GPIO_IRQ_EDGE_FALL) {
            printf("Calling button_start_pressed()\n");
            button_start_pressed();
        } else if (events & GPIO_IRQ_EDGE_RISE) {
            printf("Calling button_released()\n");
            button_released();
        }
    }

    // Обробка енкодера без антидребезгу
    if (gpio == ENCODER_CLK_PIN && encoder_active && !collecting_data) {
        bool clk_state = gpio_get(ENCODER_CLK_PIN);
        bool dt_state = gpio_get(ENCODER_DT_PIN);

        if (events & GPIO_IRQ_EDGE_FALL) {
            if (dt_state != clk_state) { // Поворот вправо
                if (encoder_slice_index < 39) encoder_slice_index++;
                printf("Encoder right: Slice %d = %u (scaled: %u)\n", 
                       encoder_slice_index, saved_slices_averages[encoder_slice_index], 
                       scale_adc_value(saved_slices_averages[encoder_slice_index]));
            } else { // Поворот вліво
                if (encoder_slice_index > 0) encoder_slice_index--;
                printf("Encoder left: Slice %d = %u (scaled: %u)\n", 
                       encoder_slice_index, saved_slices_averages[encoder_slice_index], 
                       scale_adc_value(saved_slices_averages[encoder_slice_index]));
            }
        }
    }
}


// Функція для ініціалізації всіх систем
void init_system() {
  stdio_init_all();
  init_adc();
  init_button();
  init_encoder();
  lcd_init(LCD_SDA_PIN, LCD_SCL_PIN);
}

void lcd_segment_clear() {
  for (int i=0; i<8; i++) lcd_segment[i] = 0;
}


/**
 * Перевіряє, чи є значення АЦП шумом.
 * 
 * @param value Значення АЦП.
 * @return 1 (true), якщо значення вважається шумом, інакше 0 (false).
 */
int is_noise(uint32_t value) {
  return (value < ADC_NOISE + ADC_NOISE_THRESHOLD);
}


/**
 * Масштабує середнє значення АЦП у діапазон 1-8.
 * 
 * @param average Середнє значення АЦП.
 * @return Масштабоване значення в діапазоні 1-8.
 */
int scale_adc_value(uint32_t average) {
    int max_value = 3000; // 4096
    if (average < ADC_NOISE) return 1;
    return ((average - ADC_NOISE) * 7 + (max_value - ADC_NOISE) / 2) / (max_value - ADC_NOISE) + 1;
}


/**
 * Малює вертикальний стовпчик на LCD-екрані.
 * 
 * @param lcd_segment Масив, що представляє екрани LCD, де кожен байт відповідає одному рядку.
 * @param pos Позиція стовпчика (0-4), де 0 - найлівіший стовпчик.
 * @param value Висота стовпчика (0-8), де 8 - повний стовпчик.
 * 
 * Функція малює стовпчик на екрані, де кожен піксель стовпчика представлений бітом 
 * у масиві lcd_segment. Висота стовпчика визначається параметром value, а позиція 
 * на екрані - параметром pos.
 */
void set_lcd_segment_row(int pos, int value) {
  if (pos < 0) pos = 0;
  if (pos > 4) pos = 4;

  if (value > 8) value = 8;
  if (value < 0) value = 0;

  int bit_position = 4 - pos; // Перетворення позиції в індекс біта
                              // (4-pos, бо біти нумеруються зліва направо)
  int start_row = 8 - value;  // Визначаємо, з якого рядка починати малювати
                              // стовпчик (0 - початок, 7 - кінець)
  for (int i = 7; i >= start_row; i--) {
    lcd_segment[i] |= (1 << bit_position); // Встановлюємо біт на позиції bit_position
  }
}


/**
 * Записує поточний вміст масиву lcd_segment на екран LCD.
 *
 * Ця функція створює користувацький символ на LCD на основі даних, збережених у масиві 
 * lcd_segment. Потім встановлює курсор у вказану позицію на екрані та виводить створений 
 * символ.
 *
 * @param cursor_position Вертикальна позиція (рядок) на LCD, куди буде записано символ.
 */
void lcd_segment_write(int cursor_position) {
  lcd_createChar(cursor_position, lcd_segment);
  lcd_setCursor(0, cursor_position);
  lcd_write(cursor_position);
}


uint32_t calculate_average(int from, int to) {
    uint32_t sum = 0;
    int count = 0;
    for (int i = from; i < to; i++) {
        if (is_noise(adc_values[i])) continue;
        sum += adc_values[i];
        count++;
    }
    if (count == 0) return 0; // Якщо немає допустимих значень, повертаємо 0

    return sum / count;
}


/**
 * Виводить у термінал середні значення кожного слайсу.
 * 
 * @param slices_averages Масив середніх значень слайсів.
 * @param slices_count Кількість слайсів.
 */
void print_slices_averages(uint32_t slices_averages[], int slices_count) {
    printf("Averages:\n");
    for (int i = 0; i < slices_count; i++) {
      printf(" %u=%u", slices_averages[i], scale_adc_value(slices_averages[i]));
        if ((i + 1) % 5 == 0) printf("\n"); // Друкуємо по 5 значень в рядок
    }
}


void print_effective_slices( int sample_count, int slice_length) {
  printf("Effective samples count: %d; sclice length %d\n", sample_count, slice_length);
  char buffer1[16]; sprintf(buffer1, "%d", sample_count);
  char buffer2[16]; sprintf(buffer2, "%d", slice_length);
  lcd_setCursor(1, 0); lcd_print("Cnt:"); lcd_setCursor(1, 4); lcd_print(buffer1);
  lcd_setCursor(1, 9); lcd_print("L:"); lcd_setCursor(1, 11); lcd_print(buffer2);
}


/**
 * Відображає графік на LCD, використовуючи середні значення АЦП по слайсам.
 */
void draw_graph_on_lcd() {
    int effective_samples = sample_index;
    const int total_slices = 40;
    int slice_length = effective_samples / total_slices;
    if (slice_length < 1) slice_length = 1;
    
    uint32_t slices_averages[total_slices];
    
    lcd_segment_clear();
    lcd_clear();

    for (int i = 0; i < total_slices; i++) {
        int start_idx = i * slice_length;
        int end_idx = (i + 1) * slice_length;
        if (end_idx > effective_samples) end_idx = effective_samples;
        slices_averages[i] = (uint32_t)calculate_average(start_idx, end_idx);
        saved_slices_averages[i] = slices_averages[i]; // Зберігаємо для енкодера
    }

    for (int i = 0, cursor_position = 0; i < total_slices; i++) {
        uint32_t scaled_value = scale_adc_value(slices_averages[i]);
        int lcd_segment_position = i % 5;
        set_lcd_segment_row(lcd_segment_position, scaled_value);
        if ((i + 1) % 5 == 0 || i == total_slices - 1) {
            lcd_segment_write(cursor_position++);
            lcd_segment_clear();
        }
    }
    print_slices_averages(slices_averages, total_slices);
    print_effective_slices(effective_samples, slice_length);

    encoder_active = true; // Активуємо енкодер після вимірювання
    encoder_slice_index = 0;
    printf("Encoder activated. Initial slice: %d = %u (scaled: %u)\n", 
           encoder_slice_index, saved_slices_averages[encoder_slice_index], 
           scale_adc_value(saved_slices_averages[encoder_slice_index]));
}


void lcd_hello() {
  lcd_setCursor(0, 0);
  lcd_print("Press button");
}


int main() {
    init_system();
    lcd_hello();
    while (1) {
        if (data_collection_complete) {
            encoder_active = false; // Скидаємо перед обробкою даних
            print_data();
        }
        sleep_ms(10);
    }
    return 0;
}
