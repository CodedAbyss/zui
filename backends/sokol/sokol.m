//------------------------------------------------------------------------------
//  sokol.m
//
//  When using the Metal backend, the implementation source must be
//  Objective-C (.m or .mm), but we want the samples to be in C. Thus
//  move the sokol_gfx implementation into it's own .m file.
//------------------------------------------------------------------------------
#define SOKOL_IMPL
#define SOKOL_METAL
#define SOKOL_TIME_IMPL
#define SOKOL_GLUE_IMPL
#define SOKOL_APP_IMPL
#define SOKOL_GL_IMPL
#include "sokol_gfx.h"
#include "sokol_time.h"
#include "sokol_log.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_gl.h"
