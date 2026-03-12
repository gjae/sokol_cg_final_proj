#ifndef SHADER_HPP
#define SHADER_HPP

#include "HandmadeMath.h"
#include "sokol/sokol_gfx.h"

// Uniforms parameters
struct vs_params_t {
  HMM_Mat4 view;
  HMM_Mat4 proj;
};

struct fs_params_t {
  HMM_Vec3 light_dir;
};

// Shader code for OpenGL GLSL 330
const char *vs_src = R"(
#version 330
uniform mat4 view;
uniform mat4 proj;

layout(location=0) in vec3 pos;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in mat4 instance_model;

out vec2 v_uv;
out vec3 v_normal;
out vec3 v_world_pos;

void main() {
    vec4 world_pos = instance_model * vec4(pos, 1.0);
    gl_Position = proj * view * world_pos;
    v_uv = uv;
    v_normal = mat3(instance_model) * normal;
    v_world_pos = world_pos.xyz;
}
)";

const char *fs_src = R"(
#version 330
uniform sampler2D tex;
uniform sampler2D tex_norm;
uniform sampler2D tex_orm;
uniform sampler2D tex_emit;
uniform vec3 light_dir;

in vec2 v_uv;
in vec3 v_normal;
in vec3 v_world_pos;
out vec4 frag_color;

// Helper to reconstruct tangents for normal mapping
vec3 getNormal() {
    vec3 tangentNormal = texture(tex_norm, v_uv).xyz * 2.0 - 1.0;

    vec3 q1 = dFdx(v_world_pos);
    vec3 q2 = dFdy(v_world_pos);
    vec2 st1 = dFdx(v_uv);
    vec2 st2 = dFdy(v_uv);

    vec3 N = normalize(v_normal);
    vec3 T = normalize(q1 * st2.t - q2 * st1.t);
    vec3 B = -normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);

    return normalize(TBN * tangentNormal);
}

void main() {
    vec4 base_color = texture(tex, v_uv);
    vec3 N = getNormal();
    vec3 L = normalize(-light_dir);
    
    // ORM Map: R=ambient_occlusion, G=roughness, B=metallic
    vec3 orm = texture(tex_orm, v_uv).rgb;
    float ao = orm.r;
    
    // Diffuse (directional light — no distance attenuation)
    float diff = max(dot(N, L), 0.0);
    
    float ambient = 0.35 * ao;
    float lighting = diff * 0.7 + ambient;
    
    // Emissive
    vec3 emissive = texture(tex_emit, v_uv).rgb * 2.0; // Boost emissive
    
    vec3 final_rgb = base_color.rgb * lighting + emissive;
    frag_color = vec4(final_rgb, base_color.a);
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
  desc.uniform_blocks[0].glsl_uniforms[0].glsl_name = "view";
  desc.uniform_blocks[0].glsl_uniforms[1].type = SG_UNIFORMTYPE_MAT4;
  desc.uniform_blocks[0].glsl_uniforms[1].glsl_name = "proj";

  desc.fragment_func.source = fs_src;

  // Set up uniforms for fragment shader
  desc.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
  desc.uniform_blocks[1].size = sizeof(fs_params_t);
  desc.uniform_blocks[1].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT3;
  desc.uniform_blocks[1].glsl_uniforms[0].glsl_name = "light_dir";

  // Set up images
  for (int i = 0; i < 4; i++) {
    desc.views[i].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    desc.views[i].texture.image_type = SG_IMAGETYPE_2D;
    desc.views[i].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
  }

  // Set up samplers
  desc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
  desc.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;

  // Set up image-sampler pairs
  const char *sampler_names[] = {"tex", "tex_norm", "tex_orm", "tex_emit"};
  for (int i = 0; i < 4; i++) {
    desc.texture_sampler_pairs[i].stage = SG_SHADERSTAGE_FRAGMENT;
    desc.texture_sampler_pairs[i].glsl_name = sampler_names[i];
    desc.texture_sampler_pairs[i].view_slot = i;
    desc.texture_sampler_pairs[i].sampler_slot = 0;
  }

  return sg_make_shader(&desc);
}

#endif
