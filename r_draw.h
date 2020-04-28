#ifndef R_DRAW_H
#define R_DRAW_H

#include "r.h"
#include "dstuff/math/matrix.h"
#include "spr.h"

struct vertex_t
{
    vec4_t position;
    vec4_t normal;
    vec4_t tex_coords;
};

struct r_draw_cmd_t
{
    vec2_t position;
    struct spr_sprite_h sprite;
    float scale;
    float rotation;
    uint32_t frame;
};

#define R_DRAW_CMD_BUFFER_DRAW_CMDS 65536

struct r_draw_cmd_buffer_t
{
    mat4_t view_projection_matrix;
    mat4_t view_matrix;
    uint32_t draw_cmd_count;
    struct r_draw_cmd_t draw_cmds[R_DRAW_CMD_BUFFER_DRAW_CMDS];
};

struct r_draw_data_t
{
    /* data that's written to the uniform buffer */
    mat4_t model_view_projection_matrix;
    vec4_t offset_size;
};

void r_DrawInit();

void r_DrawShutdown();

void r_BeginFrame();

void r_EndFrame();

//void r_SetViewMatrix(mat4_t *view_matrix);

//void r_SetProjectionMatrix(mat4_t *projection_matrix);

/*
=================================================================
=================================================================
=================================================================
*/

void r_BeginSubmission(mat4_t *view_matrix, mat4_t *projection_matrix);

void r_DrawSprite(struct spr_sprite_h sprite, vec2_t *position, float scale, float rotation, uint32_t frame);

void r_EndSubmission();

void r_DispatchPending();


#endif // R_DRAW_H