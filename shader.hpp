#ifndef SHADER_HPP
#define SHADER_HPP

#include "HandmadeMath.h"
#include "sokol/sokol_gfx.h"

// Uniforms parameters
struct vs_params_t {
  HMM_Mat4 view_proj;
};

// Shader code for OpenGL GLSL 330
const char *vs_src = R"(
#version 330
uniform mat4 view_proj;

layout(location=0) in vec3 pos;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in mat4 instance_model; // Using mat4 for instanced transform

out vec2 v_uv;
out vec3 v_normal;

void main() {
    gl_Position = view_proj * instance_model * vec4(pos, 1.0);
    v_uv = uv;
    v_normal = mat3(instance_model) * normal;
}
)";

const char *fs_src = R"(
#version 330
uniform sampler2D tex;

in vec2 v_uv;
in vec3 v_normal;
out vec4 frag_color;

void main() {
    // Basic lighting normal dot lightdir
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.5));
    float diff = max(dot(normalize(v_normal), light_dir), 0.1);
    
    vec4 tex_color = texture(tex, v_uv);
    frag_color = vec4(tex_color.rgb * diff, tex_color.a);
}
)";

// Function to create the basic instanced shader
inline sg_shader create_instanced_shader() {
  sg_shader_desc desc = {};
  desc.vertex_func.source = vs_src;

  // Set up uniforms
  desc.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
  desc.uniform_blocks[0].size = sizeof(vs_params_t);
  desc.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_MAT4;
  desc.uniform_blocks[0].glsl_uniforms[0].glsl_name = "view_proj";

  desc.fragment_func.source = fs_src;

  // Set up images (now called "views" in newer Sokol)
  desc.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
  desc.views[0].texture.image_type = SG_IMAGETYPE_2D;
  desc.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;

  // Set up samplers
  desc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
  desc.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;

  // Set up image-sampler pair (glsl texture)
  desc.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
  desc.texture_sampler_pairs[0].glsl_name = "tex";
  desc.texture_sampler_pairs[0].view_slot = 0;
  desc.texture_sampler_pairs[0].sampler_slot = 0;

  return sg_make_shader(&desc);
}

#endif
