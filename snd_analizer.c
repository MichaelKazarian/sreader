#include "snd_analizer.h"

const uint16_t ADC_NOISE = 2080;
const int SAMPLE_SLICE = SAMPLE_ARRAY_SIZE / GRAPH_LENGTH;
const float CONVERSION_FACTOR = 3.27f / (1 << 12);

uint16_t adc_values[SAMPLE_ARRAY_SIZE];
int sample_index = 0;
bool collecting_data = false;
bool data_collection_complete = false;
struct repeating_timer timer;
uint32_t saved_slices_averages[TOTAL_SLICES];
uint32_t saved_slices_maximums[TOTAL_SLICES];
int encoder_slice_index = 0;
bool encoder_active = false;
bool encoder_update_needed = false;

int peak_slices[TOTAL_SLICES]; // Індекси слайсів із максимумами
int peak_count = 0;            // Кількість максимумів
int current_peak_index = -1;   // Поточний індекс у peak_slices

int prev_encoder_slice_index;
int peak_durations[TOTAL_SLICES]; // Тривалості піків у мс

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

/**
 * Обробник переривань таймера
 */
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

/**
 * Обробник переривання для кнопки вимірювання
 */
void measure_pin_pressed() {
    if (!collecting_data) {
        timer_start();
    } else {
        printf("Timer already running, ignoring press\n");
    }
}

/**
 * Обробник відпускання кнопки вимірювання
 */
void measure_pin_released() {
    if (collecting_data) {
        collecting_data = false;
        data_collection_complete = true;
        timer_stop();
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

/**
 * Функція для запуску таймера
 */
void timer_start() {
  collecting_data = true;
  sample_index = 0; // Скидаємо індекс
  clear_adc_array();
  if (!add_repeating_timer_ms(-SAMPLE_INTERVAL_MS, repeating_timer_callback, NULL, &timer)) {
    printf("Failed to start timer!\n");
  } else
    printf("Data collection started.\n");
}

/**
 * Функція для зупинки таймера
 */
void timer_stop() {
  cancel_repeating_timer(&timer);
  collecting_data = false;
  // sample_index = 0; // Опціонально, якщо хочете скинути індекс
  printf("Timer stopped.\n");
}

/**
 * Функція для виводу даних
 */
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

/**
 * Функція ініціалізації АЦП
 */
void init_adc() {
  adc_init();
  adc_gpio_init(ADC_PIN);
  adc_select_input(0); // Вибираємо ADC0, який відповідає GPIO 26
}

void measure_pin_init() {
    gpio_init(MEASURE_PIN);
    gpio_set_dir(MEASURE_PIN, GPIO_IN);
    gpio_pull_up(MEASURE_PIN);
    gpio_set_irq_enabled_with_callback(MEASURE_PIN, 
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

/**
 * Перевіряє, чи минув достатній час від останньої події для уникнення дребезгу.
 * 
 * @param current_time Поточний час у мікросекундах.
 * @param last_event_time Час останньої події (змінна для оновлення).
 * @param debounce_us Мінімальний інтервал між подіями у мікросекундах.
 * @return bool True, якщо дребезг відхилено, false, якщо подія дозволена.
 */
bool debounce_check(uint64_t current_time, uint64_t* last_event_time, uint32_t debounce_us) {
    if (current_time - *last_event_time < debounce_us) {
        printf("Debounce rejected: %u\n", (uint32_t)(current_time - *last_event_time));
        return true;
    }
    *last_event_time = current_time;
    return false;
}

/**
 * Перевіряє, чи переривання викликане енкодером і чи дозволена його обробка.
 * 
 * @param gpio Номер GPIO, який викликав переривання.
 * @return bool True, якщо це енкодер і його можна обробляти, false інакше.
 */
bool is_encoder_event(uint gpio) {
    return (gpio == ENCODER_CLK_PIN && encoder_active && !collecting_data);
}

/**
 * Визначає напрямок обертання енкодера на основі стану CLK і DT.
 * 
 * @param clk_state Стан піна CLK енкодера.
 * @param dt_state Стан піна DT енкодера.
 * @return bool True, якщо поворот вправо, false, якщо вліво.
 */
bool is_encoder_rotation_right(bool clk_state, bool dt_state) {
    return (dt_state != clk_state);
}

/**
 * Обробляє поворот енкодера, оновлюючи індекс слайсу та сигналізуючи про потребу 
 * оновлення дисплея. Збільшує або зменшує encoder_slice_index залежно від напрямку 
 * обертання, визначеного станом CLK і DT.
 * 
 * @param events Події переривання (перевіряється GPIO_IRQ_EDGE_FALL).
 */
void handle_encoder_rotation(uint32_t events) {
    bool clk_state = gpio_get(ENCODER_CLK_PIN);
    bool dt_state = gpio_get(ENCODER_DT_PIN);

    if (events & GPIO_IRQ_EDGE_FALL) {
        if (is_encoder_rotation_right(clk_state, dt_state)) {
            if (encoder_slice_index < 39) encoder_slice_index++;
        } else {
            if (encoder_slice_index > 0) encoder_slice_index--;
        }
        encoder_update_needed = true;
        /* printf("Encoder slice %d\n", encoder_slice_index); */
    }
}

/**
 * Перевіряє, чи переривання викликане кнопкою.
 * 
 * @param gpio Номер GPIO, який викликав переривання.
 * @return bool True, якщо це кнопка, false інакше.
 */
bool is_measure_pin_event(uint gpio) {
    return (gpio == MEASURE_PIN);
}

/**
 * Обробляє подію кнопки з антидребезгом, виводячи стан і викликаючи відповідні дії.
 * Якщо антидребезг відхиляє подію, обробка припиняється. Інакше визначається, чи це 
 * натискання (FALL) чи відпускання (RISE), і викликається відповідна функція.
 * 
 * @param current_time Поточний час у мікросекундах.
 * @param last_event_time Час останньої події (змінна для оновлення).
 * @param events Події переривання (FALL або RISE).
 */
void handle_measure_pin_event(uint64_t current_time, uint64_t* last_event_time, uint32_t events) {
    /* if (debounce_check(current_time, last_event_time, BUTTON_DEBOUNCE_US)) { return; } */
    printf("Event: %s, Time diff: %u\n", 
           (events & GPIO_IRQ_EDGE_FALL) ? "FALL" : "RISE", 
           (uint32_t)(current_time - *last_event_time));
    if (events & GPIO_IRQ_EDGE_FALL) {
        printf("Calling measure_pin_pressed()\n");
        measure_pin_pressed();
    } else if (events & GPIO_IRQ_EDGE_RISE) {
        printf("Calling measure_pin_released()\n");
        measure_pin_released();
    }
}

/**
 * Обробляє переривання від GPIO, розподіляючи їх між кнопкою та енкодером.
 * Для кнопки перевіряється антидребезг і викликається відповідна логіка.
 * Для енкодера обробляється поворот без антидребезгу, оновлюється індекс слайсу.
 * 
 * @param gpio Номер GPIO, який викликав переривання.
 * @param events Тип події (FALL або RISE).
 */
void gpio_interrupt_handler(uint gpio, uint32_t events) {
    static uint64_t last_button_event_time = 0;
    uint64_t current_time = time_us_64();

    if (is_measure_pin_event(gpio)) {
        handle_measure_pin_event(current_time, &last_button_event_time, events);
    }

    if (is_encoder_event(gpio)) {
        handle_encoder_rotation(events);
    }

    if (gpio == NEXT_PEAK_PIN && (events & GPIO_IRQ_EDGE_FALL)) {
        move_to_next_peak();
    }
}

/**
 * Функція для ініціалізації всіх систем
 */
void init_system() {
  stdio_init_all();
  init_adc();
  measure_pin_init();
  init_encoder();
  init_next_peak_pin();
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
    int max_value = 3200; // 4096
    if (average < ADC_NOISE) return 1;
    return ((average - ADC_NOISE) * 7 + (max_value - ADC_NOISE) / 2) / (max_value - ADC_NOISE) + 1;
}

/**
 * Обробляє піксель-вказівник для стовпчика на LCD.
 * @param bit_position Позиція біта в байті (0–4), що відповідає стовпчику.
 * @param value Висота стовпчика (0–7).
 */
void handle_pointer_pixel(int bit_position, int value) {
#if POINTER_POSITION == 7
    // Вимикаємо нижній піксель, рядок 7, якщо він був увімкнений
    if (value > 0) {
        lcd_segment[7] &= ~(1 << bit_position);
    }
#elif POINTER_POSITION == 0
    // Вмикаємо верхній піксель, рядок 0
    lcd_segment[0] |= (1 << bit_position);
#else
#error "POINTER_POSITION must be 7 (bottom) or 0 (top)"
#endif
}

/**
 * Малює вертикальний стовпчик на LCD-екрані.
 * @param pos Позиція стовпчика (0–4).
 * @param value Висота стовпчика (0–7).
 * @param enable_pointer Чи вмикати/вимикати піксель-вказівник (залежно від POINTER_POSITION).
 */
void set_lcd_segment_row(int pos, int value, bool enable_pointer) {
    if (pos < 0) pos = 0;
    if (pos > 4) pos = 4;
    if (value > 7) value = 7;  // Максимальна висота — 7, а не 8
    if (value < 0) value = 0;

    int bit_position = 4 - pos; // Позиція біта в байті (зліва направо)
    int start_row = 8 - value;  // Початок малювання стовпчика

    for (int i = 7; i >= start_row; i--) { // Заповнюємо стовпчик відповідно до висоти
        lcd_segment[i] |= (1 << bit_position); // Увімкнемо біт
    }

    if (enable_pointer) {
        handle_pointer_pixel(bit_position, value);
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

/**
 * Виводить кількість зібраних зразків (sample_count) і довжину слайсу (slice_length)
 * на LCD у рядку 0, починаючи з позиції 8. Якщо рядок коротший за 8 символів,
 * він зсувається максимально вправо так, щоб останній символ був на позиції 15.
 * Значення розділяються символом '/'.
 *
 * @param sample_count Кількість зібраних зразків (макс. 4000, 4 символи).
 * @param slice_length Довжина одного слайсу (макс. 100, 3 символи).
 */
void display_slice_info(int sample_count, int slice_length) {
  printf("Effective samples count: %d; slice length %d\n", sample_count, slice_length);

  char buffer[9]; // 8 символів + \0, для макс. "4000/100"
  sprintf(buffer, "%d/%d", sample_count, slice_length);

  int len = strlen(buffer);
  int start_pos = (len < 8) ? (15 - len + 1) : 8; // Зсув до позиції 15, якщо<8

  lcd_setCursor(0, start_pos);
  lcd_print(buffer);
}

void calculate_slice_averages(int effective_samples, int slice_length, 
                              uint32_t* slices_averages, uint32_t* saved_slices_averages) {
  for (int i = 0; i < TOTAL_SLICES; i++) {
    int start_idx = i * slice_length;
    int end_idx = (i + 1) * slice_length;
    if (end_idx > effective_samples) end_idx = effective_samples;

    uint32_t sum = 0;
    uint32_t max = 0;
    int count = 0;
    for (int j = start_idx; j < end_idx; j++) {
      if (!is_noise(adc_values[j])) {
        sum += adc_values[j];
        if (adc_values[j] > max) max = adc_values[j];
        count++;
      }
    }
    slices_averages[i] = (count > 0) ? (sum / count) : 0;
    saved_slices_averages[i] = slices_averages[i];
    saved_slices_maximums[i] = max;
  }
}

/**
 * Відображає графік на LCD, масштабуючи середні значення слайсів і записуючи їх
 * у сегменти дисплея. Кожні 5 слайсів формують один символ, який записується на дисплей.
 *
 * @param slices_averages Масив середніх значень слайсів для масштабування та відображення.
 */
void display_graph(uint32_t* slices_averages) {
    for (int i = 0, cursor_position = 0; i < TOTAL_SLICES; i++) {
        uint32_t scaled_value = scale_adc_value(slices_averages[i]);
        int lcd_segment_position = i % GRAPH_SLICE_LENGTH;
        set_lcd_segment_row(lcd_segment_position, scaled_value, false); // Без вимкнення
        if ((i + 1) % GRAPH_SLICE_LENGTH == 0 || i == TOTAL_SLICES - 1) {
            lcd_segment_write(cursor_position++);
            lcd_segment_clear();
        }
    }
}

/**
 * Відображає графік на LCD, використовуючи середні значення АЦП по слайсам.
 */
void draw_graph_on_lcd() {
    int effective_samples = sample_index;
    int slice_length = effective_samples / TOTAL_SLICES;
    if (slice_length < 1) slice_length = 1;

    uint32_t slices_averages[TOTAL_SLICES];

    lcd_segment_clear();
    lcd_clear();

    calculate_slice_averages(effective_samples, slice_length, slices_averages, saved_slices_averages);
    display_graph(slices_averages);
    print_slices_averages(slices_averages, TOTAL_SLICES);
    display_slice_info(effective_samples, slice_length);

    analyze_peaks(slice_length);
    
    // Виведення кількості максимумів (2 символи)
    char buffer[3];
    sprintf(buffer, "%d", peak_count);
    lcd_setCursor(1, 0);
    lcd_print(buffer);
    
    encoder_active = true;
    encoder_slice_index = 0;
    prev_encoder_slice_index = -1; // Скидаємо попередній індекс
    /* printf("Encoder activated. Initial slice: %d = %u (scaled: %u)\n",  */
    /*        encoder_slice_index, saved_slices_averages[encoder_slice_index],  */
    /*        scale_adc_value(saved_slices_averages[encoder_slice_index])); */
}

void lcd_hello() {
  lcd_setCursor(0, 0);
  lcd_print("Press button");
}

/**
 * Оновлює символ на LCD, вимикаючи піксель-вказівник для поточного слайсу.
 * @param slice_index Індекс поточного слайсу (0–39).
 * @param prev_slice_index Індекс попереднього слайсу (для відновлення).
 */
void update_slice_column(int slice_index, int prev_slice_index) {
    int cursor_position = slice_index / GRAPH_SLICE_LENGTH; // Номер символу (0–7)
    int lcd_segment_position = slice_index % GRAPH_SLICE_LENGTH; // Позиція стовпчика (0–4)

    lcd_segment_clear();

    for (int i = 0; i < GRAPH_SLICE_LENGTH; i++) {
        int current_slice = cursor_position * GRAPH_SLICE_LENGTH + i;
        if (current_slice >= TOTAL_SLICES) break;
        uint32_t height = scale_adc_value(saved_slices_averages[current_slice]);
        // Вимикаємо піксель-вказівник тільки для активного слайсу
        set_lcd_segment_row(i, height, i == lcd_segment_position);
    }

    lcd_segment_write(cursor_position);

    if (prev_slice_index >= 0 && (prev_slice_index / GRAPH_SLICE_LENGTH) != cursor_position) {
        int prev_cursor_position = prev_slice_index / GRAPH_SLICE_LENGTH;
        lcd_segment_clear();
        for (int i = 0; i < GRAPH_SLICE_LENGTH; i++) {
            int current_slice = prev_cursor_position * GRAPH_SLICE_LENGTH + i;
            if (current_slice >= TOTAL_SLICES) break;
            uint32_t height = scale_adc_value(saved_slices_averages[current_slice]);
            set_lcd_segment_row(i, height, false); // Без вимкнення пікселя
        }
        lcd_segment_write(prev_cursor_position);
    }
}

/**
 * Перевіряє, чи потрібно оновити значення енкодера на LCD. 
 * Повертає true, якщо енкодер активний, вимірювання не проводиться, 
 * і є запит на оновлення дисплея.
 * 
 * @return bool Чи потрібно оновити LCD.
 */
bool should_update_encoder_display() {
    return encoder_update_needed && encoder_active && !collecting_data;
}

void update_encoder_display() {
    float volts_value = saved_slices_averages[encoder_slice_index] * CONVERSION_FACTOR;
    float max_value = saved_slices_maximums[encoder_slice_index] * CONVERSION_FACTOR;

    char buffer[14];
    sprintf(buffer, "%2d/%.3f/%.3f", encoder_slice_index + 1, volts_value, max_value);
    int start_pos = 2;
    lcd_setCursor(1, start_pos);
    lcd_print(buffer);
    display_peak_info();
    encoder_update_needed = false;

    // Оновлюємо стовпчик для поточного і попереднього слайсу
    update_slice_column(encoder_slice_index, prev_encoder_slice_index);
    prev_encoder_slice_index = encoder_slice_index; // Зберігаємо поточний індекс як попередній
}

void display_peak_info() {
    lcd_setCursor(0, 8);
    lcd_print("        ");
    display_slice_info(peak_slices[current_peak_index]+1,
                       peak_durations[current_peak_index]);
}

void analyze_peaks(int slice_length) {
    peak_count = 0;
    const float PEAK_THRESHOLD = 2.50f; // Поріг значущості піку

    for (int i = 0; i < TOTAL_SLICES; i++) {
        float avg_volt = saved_slices_averages[i] * CONVERSION_FACTOR;

        if (avg_volt > PEAK_THRESHOLD) {
            if (peak_count < TOTAL_SLICES) {
                peak_slices[peak_count] = i;

                int slice_start = i * slice_length;
                int slice_end = (i + 1) * slice_length - 1;
                if (slice_end >= SAMPLE_ARRAY_SIZE) slice_end = SAMPLE_ARRAY_SIZE - 1;

                int peak_start = slice_start;
                int peak_end = slice_end;

                while (peak_start > 0 && 
                       (adc_values[peak_start - 1] * CONVERSION_FACTOR) > PEAK_THRESHOLD) {
                    peak_start--;
                }

                while (peak_end < SAMPLE_ARRAY_SIZE - 1 && 
                       (adc_values[peak_end + 1] * CONVERSION_FACTOR) > PEAK_THRESHOLD) {
                    peak_end++;
                }

                int duration = peak_end - peak_start + 1; // У записах
                int duration_ms = duration; // У мс (1 запис = 1 мс)
                if (duration_ms < MIN_PEAK_DURATION) duration_ms = MIN_PEAK_DURATION;

                peak_durations[peak_count] = duration_ms; // Зберігаємо тривалість
                printf("Peak at slice %d: %.3f V, duration %d ms\n", 
                       i, avg_volt, duration_ms);

                peak_count++;

                int next_slice = (peak_end + 1) / slice_length;
                if (next_slice > i) i = next_slice - 1;
            }
        }
    }

    printf("Found %d peaks: ", peak_count);
    for (int j = 0; j < peak_count; j++) {
        printf("%d ", peak_slices[j]);
    }
    printf("\n");
}

void init_next_peak_pin() {
    gpio_init(NEXT_PEAK_PIN);
    gpio_set_dir(NEXT_PEAK_PIN, GPIO_IN);
    gpio_pull_up(NEXT_PEAK_PIN);
    gpio_set_irq_enabled_with_callback(NEXT_PEAK_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_interrupt_handler);
}

void move_to_next_peak() {
    if (peak_count == 0) return;
    current_peak_index = (current_peak_index + 1) % peak_count;
    encoder_slice_index = peak_slices[current_peak_index];
    int duration = peak_durations[current_peak_index];
    encoder_update_needed = true;
    printf("Moved to peak at slice %d, value %d\n", encoder_slice_index, duration);
}

int main() {
    init_system();
    lcd_hello();
    while (1) {
        if (data_collection_complete) {
            encoder_active = false;
            print_data();
        }
        if (should_update_encoder_display()) {
            update_encoder_display();
        }
        sleep_ms(10);
    }
    return 0;
}
