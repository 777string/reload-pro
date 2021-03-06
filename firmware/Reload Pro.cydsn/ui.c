/* ========================================
 *
 * Copyright Arachnid Labs, 2013
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * CONFIDENTIAL AND PROPRIETARY INFORMATION
 * WHICH IS THE PROPERTY OF your company.
 *
 * ========================================
*/

#include "tasks.h"
#include "Display_font.h"
#include "config.h"
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <stdio.h>
#include <stdlib.h>

xQueueHandle ui_queue;

typedef struct {
	void (*func)(char *);
	char *label;
} readout_function_impl;

const display_settings_t display_settings = {
	.cc = {
		.readouts = {READOUT_CURRENT_SETPOINT, READOUT_CURRENT_USAGE, READOUT_VOLTAGE},
	},
};

typedef struct state_func_t {
	struct state_func_t (*func)(const void*);
	const void *arg;
	const int8 is_main_state;
} state_func;

typedef enum {
	VALUE_TYPE_CURRENT_RANGE,
} value_type;

typedef struct {
	const char *caption;
	const state_func new_state;
} menuitem;

typedef struct {
	const char *title;
	const menuitem items[];
} menudata;

typedef struct {
	const value_type type;
	const void *target;
	const int value;
} valueconfig;

static state_func cc_load(const void*);
static state_func menu(const void*);
static state_func calibrate(const void*);
static state_func display_config(const void*);
static state_func set_contrast(const void *);
static state_func overtemp(const void*);

#define STATE_MAIN {NULL, NULL, 0}
#define STATE_CC_LOAD {cc_load, NULL, 1}
#define STATE_CALIBRATE {calibrate, NULL, 0}
#define STATE_CONFIGURE_CC_DISPLAY {display_config, &display_settings.cc, 0}
#define STATE_SET_CONTRAST {set_contrast, NULL, 0}
#define STATE_OVERTEMP {overtemp, NULL, 0}

#ifdef USE_SPLASHSCREEN
static state_func splashscreen(const void*);
#define STATE_SPLASHSCREEN {splashscreen, NULL, 0}
#endif

const menudata set_readout_menu = {
	"Choose value",
	{
		{"Set Current", {NULL, (void*)READOUT_CURRENT_SETPOINT, 0}},
		{"Act. Current", {NULL, (void*)READOUT_CURRENT_USAGE, 0}},
		{"Voltage", {NULL, (void*)READOUT_VOLTAGE, 0}},
		{"Power", {NULL, (void*)READOUT_POWER, 0}},
		{"Resistance", {NULL, (void*)READOUT_RESISTANCE, 0}},
		{"None", {NULL, (void*)READOUT_NONE, 0}},
		{NULL, {NULL, NULL, 0}},
	}
};

const menudata choose_readout_menu = {
	"Readouts",
	{
		{"Main display", {NULL, (void*)0, 0}},
		{"Left display", {NULL, (void*)1, 0}},
		{"Right display", {NULL, (void*)2, 0}},
		{NULL, {NULL, NULL, 0}},
	}
};

const menudata main_menu = {
	NULL,
	{
		{"C/C Load", STATE_CC_LOAD},
		{"Readouts", STATE_CONFIGURE_CC_DISPLAY},
		{"Contrast", STATE_SET_CONTRAST},
		{"Calibrate", STATE_CALIBRATE},
		{NULL, {NULL, NULL, 0}},
	}
};

#define STATE_MAIN_MENU {menu, &main_menu, 0}

CY_ISR(button_press_isr) {
	static ui_event event = {.type = UI_EVENT_BUTTONPRESS, .when = 0};
	event.int_arg = QuadButton_Read();
	QuadButton_ClearInterrupt();
	
	portTickType now = xTaskGetTickCountFromISR();
	if(now - event.when > configTICK_RATE_HZ / 10) {
		event.when = now;
		xQueueSendToBackFromISR(ui_queue, &event, NULL);
	}
}

// Maps current state (index) to next state for a forward transition.
const int8 quadrature_states[] = {0x1, 0x3, 0x0, 0x2};

CY_ISR(quadrature_event_isr) {
	static int8 last_levels = 3;
	static int8 count = 0;
	
	int levels = Quadrature_Read();
	Quadrature_ClearInterrupt();

	if(quadrature_states[last_levels] == levels) {
		count += 1;
		last_levels = levels;
	} else if(quadrature_states[levels] == last_levels) {
		count -= 1;
		last_levels = levels;
	}
	
	if(abs(count) >= 4) {
		ui_event event = {.type = UI_EVENT_UPDOWN, .when = xTaskGetTickCountFromISR(), .int_arg = count / 4};
		xQueueSendToBackFromISR(ui_queue, &event, NULL);
		count = count % 4;
	}
}

static void format_number(int num, const char suffix, char *out) {
	if(num < 0)
		num = 0;
	
	int magnitude = 1;
	while(num >= 1000000) {
		num /= 1000;
		magnitude++;
	}
	
	int whole = num / 1000, remainder = num % 1000;
	if(whole < 10) {
		// Format: x.xx
		sprintf(out, "%1d.%02d", whole, remainder / 10);
	} else if(whole < 100) {
		// Format: xx.x
		sprintf(out, "%02d.%1d", whole, remainder / 100);
	} else {
		// Format: xxx
		sprintf(out, "%03d", whole);
	}
	
	if(magnitude == 1) {
		strcat(out, (const char[]){'m', suffix, '\0'});
	} else {
		strcat(out, (const char[]){suffix, ' ', '\0'});
	}
}

static void adjust_current_setpoint(int delta) {
	if(state.current_range == 0) {
		set_current(state.current_setpoint + delta * CURRENT_LOWRANGE_STEP);
	} else {
		set_current(state.current_setpoint + delta * CURRENT_FULLRANGE_STEP);
	}
}

static void next_event(ui_event *event) {
	static portTickType last_tick = 0;
	
	portTickType now = xTaskGetTickCount();
	if(now > last_tick + configTICK_RATE_HZ / 10 || !xQueueReceive(ui_queue, event, configTICK_RATE_HZ / 10 - (now - last_tick))) {
		event->type = UI_EVENT_ADC_READING;
		event->when = now;
		last_tick = now;
	}
}

static void draw_menu(const menudata *menu, int selected) {
	int start_row = 0;
	int height = 4;

	if(menu->title) {
		int8 padding = (160 - strlen(menu->title) * 12) / 2;
		Display_Clear(0, 0, 2, padding, 0xFF);
		Display_DrawText(0, padding, menu->title, 1);
		Display_Clear(0, 160 - padding, 2, 160, 0xFF);
		start_row++;
		height--;
	}

	Display_DrawText(start_row * 2, 148, ((selected / height) > 0)?FONT_GLYPH_UARR:" ", 0);
	
	// Find the block of items the selected element is in
	const menuitem *current = &menu->items[selected - selected % height];
	selected %= height;
	
	for(int i = 0; i < height; i++) {
		if(current->caption != NULL) {
			Display_DrawText((i + start_row) * 2, 0, current->caption, i == selected);
			Display_Clear((i + start_row) * 2, strlen(current->caption) * 12, (i + start_row + 1) * 2, 142, (i == selected)*255);
			current++;
		} else {
			Display_Clear((i + start_row) * 2, 0, (i + start_row + 1) * 2, 160, 0);
		}
	}
	
	if(current->caption != NULL) {
		Display_DrawText(6, 148, FONT_GLYPH_DARR, 0);
	} else {
		Display_DrawText(6, 148, " ", 0);
	}
}

void print_nothing(char *buf) {
	strcpy(buf, "      ");
}

void print_setpoint(char *buf) {
	format_number(get_current_setpoint(), 'A', buf);
}

void print_current_usage(char *buf) {
	format_number(get_current_usage(), 'A', buf);
}

void print_voltage(char *buf) {
	format_number(get_voltage(), 'V', buf);
}

void print_power(char *buf) {
	int power = (get_current_usage() / 1000) * (get_voltage() / 1000);
	format_number(power, 'W', buf);
}

void print_resistance(char *buf) {
	int current = get_current_usage();
	if(current > 0) {
		format_number(((get_voltage() * 10) / (current / 100000)), GLYPH_CHAR(FONT_GLYPH_OHM), buf);
	} else {
		strcpy(buf, "----" FONT_GLYPH_OHM);
	}
}

const readout_function_impl readout_functions[] = {
	{NULL, ""},
	{print_setpoint, "SET"},
	{print_current_usage, "ACT"},
	{print_voltage, ""},
	{print_power, ""},
	{print_resistance, ""},
};

static void draw_status(const display_config_t *config) {
	char buf[8];

	// Draw the main info
	const readout_function_impl *readout = &readout_functions[config->readouts[0]];
	if(readout->func != print_nothing) {
		readout->func(buf);
		strcat(buf, " ");
		Display_DrawBigNumbers(0, 0, buf);
		if(strchr(buf, '.') == NULL)
			// Clear any detritus left over from longer strings
			Display_Clear(0, 108, 4, 120, 0);
	} else {
		Display_Clear(0, 0, 6, 120, 0);
		Display_Clear(4, 120, 6, 160, 0);
	}

	// Draw the type in the top right
	uint8 labelsize = strlen(readout->label) * 12;
	Display_DrawText(0, 160 - labelsize, readout->label, 1);
	if(labelsize < 36)
		Display_Clear(0, 124, 2, 160 - labelsize, 0);

	// Draw the two smaller displays
	for(int i = 0; i < 2; i++) {
		readout = &readout_functions[config->readouts[i + 1]];
		readout->func(buf);
		if(strlen(buf) == 5)
			strcat(buf, " ");
		Display_DrawText(6, 88 * i, buf, 0);
	}
}

static state_func display_config(const void *arg) {
	display_config_t *config = (display_config_t*)arg;
	
	state_func display = menu(&choose_readout_menu);
	if(display.func == overtemp)
		return display;
	
	state_func readout = menu(&set_readout_menu);
	if(readout.func == overtemp)
		return readout;
	
	EEPROM_Write((uint8*)&((readout_function){(readout_function)readout.arg}), (uint8*)&config->readouts[(int)display.arg], sizeof(readout_function));
	
	return (state_func)STATE_MAIN;
}

static state_func set_contrast(const void *arg) {
	Display_ClearAll();
	Display_Clear(0, 0, 2, 160, 0xFF);
	Display_DrawText(0, 32, "Contrast", 1);
	Display_DrawText(6, 38, FONT_GLYPH_ENTER ": Done", 0);

	// Left and right ends of the bar
	Display_Clear(4, 15, 5, 16, 0xFF);
	Display_Clear(4, 145, 5, 146, 0xFF);
	
	int contrast = settings->lcd_contrast;
	ui_event event;
	while(1) {
		Display_Clear(4, 16, 5, 16 + contrast * 2, 0xFF);
		Display_Clear(4, 16 + contrast * 2, 5, 145, 0x81);
		
		next_event(&event);
		switch(event.type) {
		case UI_EVENT_UPDOWN:
			contrast += event.int_arg;
			if(contrast > 0x3F) {
				contrast = 0x3F;
			} else if(contrast < 0) {
				contrast = 0;
			}
			Display_SetContrast(contrast);
			break;
		case UI_EVENT_BUTTONPRESS:
			if(event.int_arg == 1) {
				EEPROM_Write((const uint8*)&contrast, (const uint8*)&settings->lcd_contrast, sizeof(int));
				return (state_func)STATE_MAIN;
			}
			break;
		case UI_EVENT_OVERTEMP:
			return (state_func)STATE_OVERTEMP;
		default:
			break;
		}
	}
}

static state_func overtemp(const void *arg) {
	Display_Clear(0, 0, 8, 160, 0xFF);
	Display_DrawText(2, 6, "! OVERTEMP !", 1);
	Display_DrawText(6, 32, FONT_GLYPH_ENTER ": Reset", 1);
	
	ui_event event;
	event.type = UI_EVENT_NONE;
	while(event.type != UI_EVENT_BUTTONPRESS || event.int_arg != 1) {
		next_event(&event);
		if(get_output_mode() == OUTPUT_MODE_FEEDBACK)
			return (state_func)STATE_MAIN;
	}
		
	set_current(0);
	set_output_mode(OUTPUT_MODE_FEEDBACK);
	return (state_func)STATE_MAIN;
}

static state_func menu(const void *arg) {
	const menudata *menu = (const menudata *)arg;
	
	Display_ClearAll();
	
	int selected = 0;
	ui_event event;
	event.type = UI_EVENT_NONE;
	while(event.type != UI_EVENT_BUTTONPRESS || event.int_arg != 1) {
		draw_menu(menu, selected);
		next_event(&event);
		switch(event.type) {
		case UI_EVENT_UPDOWN:
			if(event.int_arg < 0) {
				// Move up the menu
				if(selected + event.int_arg >= 0) {
					selected += event.int_arg;
				} else {
					selected = 0;
				}
			} else {
				// Move down the menu (but not past the end)
				for(int i = 0; i < event.int_arg; i++) {
					if(menu->items[selected + 1].caption == NULL)
						break;
					selected++;
				}
			}
			break;
		case UI_EVENT_OVERTEMP:
			return (state_func)STATE_OVERTEMP;
		default:
			break;
		}
	}

	return menu->items[selected].new_state;
}

#ifdef USE_SPLASHSCREEN
static state_func splashscreen(const void *arg) {
	vTaskDelay(configTICK_RATE_HZ * 3);
	return (state_func)STATE_CC_LOAD;
}
#endif

static state_func cc_load(const void *arg) {
	Display_ClearAll();
	
	ui_event event;
	while(1) {
		next_event(&event);
		switch(event.type) {
		case UI_EVENT_BUTTONPRESS:
			if(event.int_arg == 1)
				return (state_func)STATE_MAIN_MENU;
		case UI_EVENT_UPDOWN:
			adjust_current_setpoint(event.int_arg);
			break;
		case UI_EVENT_OVERTEMP:
			return (state_func)STATE_OVERTEMP;
		default:
			break;
		}
		draw_status(&display_settings.cc);
		//CyDelay(200);
	}
}

// Calibrates the ADC voltage and current offsets.
// Run with nothing attached to the terminals.
static void calibrate_offsets(settings_t *new_settings) {
	Display_DrawText(2, 0, "  1: Offset  ", 1);
	Display_DrawText(6, 38, FONT_GLYPH_ENTER ": Next", 0);

	// Wait for a button press
	ui_event event;
	event.type = UI_EVENT_NONE;
	while(event.type != UI_EVENT_BUTTONPRESS || event.int_arg != 1)
		next_event(&event);
	
	new_settings->adc_voltage_offset = get_raw_voltage();
	new_settings->adc_current_offset = get_raw_current_usage();
}

// Calibrate the ADC voltage gain.
// Run with a known voltage across the terminals
static void calibrate_voltage(settings_t *new_settings) {
	Display_DrawText(2, 0, "  2: Voltage ", 1);

	ui_event event;
	char buf[8];
	event.type = UI_EVENT_NONE;
	while(event.type != UI_EVENT_BUTTONPRESS || event.int_arg != 1) {
		next_event(&event);
		
		format_number((get_raw_voltage() - new_settings->adc_voltage_offset) * new_settings->adc_voltage_gain, 'V', buf);
		strcat(buf, " ");
		Display_DrawText(4, 43, buf, 0);
		
		switch(event.type) {
		case UI_EVENT_UPDOWN:
			new_settings->adc_voltage_gain += (new_settings->adc_voltage_gain * event.int_arg) / 500;
			break;
		default:
			break;
		}
	}
}

// Calibrates the opamp and current DAC offsets.
// Run with a voltage source attached
static void calibrate_opamp_dac_offsets(settings_t *new_settings) {
	Display_Clear(2, 0, 8, 160, 0);
	Display_DrawText(4, 12, "Please wait", 0);
	set_current(100000);

	// Find the best setting for the opamp trim
	for(int i = 0; i < 32; i++) {
		CY_SET_REG32(Opamp_cy_psoc4_abuf__OA_OFFSET_TRIM, i);
		CyDelay(10);
		
//		ADC_EnableInjection();
		ADC_IsEndConversion(ADC_WAIT_FOR_RESULT_INJ);
		int offset = ADC_GetResult16(ADC_CHAN_CURRENT_SENSE) - ADC_GetResult16(ADC_CHAN_CURRENT_SET);
		if(offset <= 0) {
			new_settings->opamp_offset_trim = i - 1;
			break;
		}
	}
	
	set_current(0);
	// Find the best setting for the DAC offsets
	/*for(int i = 0; i < 2; i++) {
		set_current_range(i);
		new_settings->dac_offsets[i] =  0;
		for(int j = 0; j < 256; j++) {
			IDAC_SetValue(j);
			CyDelay(100);

			int offset = ADC_GetResult16(ADC_CHAN_CURRENT_SENSE) - new_settings->adc_current_offset;
			if(offset > 0)
				break;
			new_settings->dac_offsets[i] = -j;
		}
	}*/
}

static void calibrate_current(settings_t *new_settings) {
	Display_Clear(4, 0, 8, 160, 0);
	Display_DrawText(2, 0, "  3: Current ", 1);
	Display_DrawText(6, 38, FONT_GLYPH_ENTER ": Next", 0);
	
/*	set_current_range(1);
	IDAC_SetValue(42 + new_settings->dac_offsets[1]);
	
	ui_event event;
	char buf[8];
	int current;
	
	event.type = UI_EVENT_NONE;
	while(event.type != UI_EVENT_BUTTONPRESS || event.int_arg != 1) {
		next_event(&event);
		
		current = (get_raw_current_usage() - new_settings->adc_current_offset) * new_settings->adc_current_gain;
		format_number(current, 'A', buf);
		strcat(buf, " ");
		Display_DrawText(4, 43, buf, 0);
		
		switch(event.type) {
		case UI_EVENT_UPDOWN:
			new_settings->adc_current_gain += (new_settings->adc_current_gain * event.int_arg) / 500;
			break;
		default:
			break;
		}
	}
	
	new_settings->dac_gains[1] = current / 42;
	set_current_range(0);
	IDAC_SetValue(200 + new_settings->dac_offsets[0]);
	CyDelay(100);
	current = (ADC_GetResult16(ADC_CHAN_CURRENT_SENSE) - new_settings->adc_current_offset) * new_settings->adc_current_gain;
	new_settings->dac_gains[0] = current / 200;
	
	IDAC_SetValue(new_settings->dac_offsets[0]);*/
}

state_func calibrate(const void *arg) {
	set_current(0);
	
	settings_t new_settings;
	memcpy(&new_settings, settings, sizeof(settings_t));
	
	Display_ClearAll();
	Display_DrawText(0, 0, " CALIBRATION ", 1);
	
	calibrate_offsets(&new_settings);
	calibrate_voltage(&new_settings);
	calibrate_opamp_dac_offsets(&new_settings);
	calibrate_current(&new_settings);
	
	EEPROM_Write((uint8*)&new_settings, (uint8*)settings, sizeof(settings_t));
	
	return (state_func){NULL, NULL, 0};
}

void vTaskUI( void *pvParameters ) {
	ui_queue = xQueueCreate(2, sizeof(ui_event));

	QuadratureISR_StartEx(quadrature_event_isr);
	QuadButtonISR_StartEx(button_press_isr);

	state_func main_state = STATE_CC_LOAD;
	#ifdef USE_SPLASHSCREEN
	state_func state = STATE_SPLASHSCREEN;
	#else
	state_func state = STATE_CC_LOAD;
	#endif
	
	while(1) {
		state_func new_state = state.func(state.arg);
		if(new_state.func == NULL) {
			memcpy(&state, &main_state, sizeof(state_func));
		} else {
			memcpy(&state, &new_state, sizeof(state_func));
		}
		
		if(state.is_main_state) {
			memcpy(&main_state, &state, sizeof(state_func));
		}
	}
}


/* [] END OF FILE */
