#pragma once

#include <obs-module.h>

#include <cstdint>

struct openvr_capture_handle;

openvr_capture_handle *openvr_capture_create(obs_source_t *source);
void openvr_capture_destroy(openvr_capture_handle *capture);
void openvr_capture_configure(openvr_capture_handle *capture, int eye,
			      bool fill_canvas, double crop_top,
			      double crop_right, double crop_bottom,
			      double crop_left);
void openvr_capture_show(openvr_capture_handle *capture);
void openvr_capture_hide(openvr_capture_handle *capture);
void openvr_capture_tick(openvr_capture_handle *capture, float seconds);
void openvr_capture_render(openvr_capture_handle *capture, gs_effect_t *effect);
uint32_t openvr_capture_width(const openvr_capture_handle *capture);
uint32_t openvr_capture_height(const openvr_capture_handle *capture);
bool openvr_capture_ready(const openvr_capture_handle *capture);

void register_openvr_capture_source();
void shutdown_openvr_capture_runtime();
