#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_time.h"

#include "camera.hpp"
#include "game.hpp"
#include "obj_loader.hpp"
#include "shader.hpp"
#include <cstdio>
#include <vector>

// Estado global del juego
static GameConfig g_config;
static Camera g_camera;
static uint64_t last_time = 0;

struct InputState {
  bool keys[512]; // Keep track of pressed keys
};
static InputState g_input = {};

// Recursos de Sokol
static Mesh g_wall_mesh;
static Mesh g_floor_mesh;
static sg_pipeline g_pip;
static sg_buffer g_inst_buf;
static sg_buffer g_floor_inst_buf;
static int g_num_instances = 0;
static int g_num_floor_instances = 0;
static sg_sampler g_sampler;

// Callback: se ejecuta una vez al iniciar la aplicación
static void init_cb(void) {
  sg_desc desc = {};
  desc.environment = sglue_environment();
  desc.logger.func = slog_func;
  sg_setup(&desc);

  stm_setup();

  // Generar niveles y colectables
  int base_size = 10;
  create_levels(g_config.levels, base_size);

  for (int i = 0; i < 3; i++) {
    g_config.collectable_count[i] = place_collectables(
        g_config.collectables[i], g_config.levels[i].map->rooms,
        g_config.levels[i].map->room_count,
        g_config.levels[i].map->get_matrix());
  }

  g_config.current_level = 0;

  printf("Juego inicializado - Nivel %d (mapa %dx%d) - %d colectables\n",
         g_config.levels[0].level, g_config.levels[0].map->get_size(),
         g_config.levels[0].map->get_size(), g_config.collectable_count[0]);

  // Posicionar la cámara en una sala aleatoria
  if (g_config.levels[0].map->room_count > 0) {
    int start_room_idx = rand() % g_config.levels[0].map->room_count;
    printf("Sala inicial: %d\n", start_room_idx);
    Room start_room = g_config.levels[0].map->rooms[start_room_idx];
    float cw = 2.0f; // Tamaño aproximado de la celda en 3D
    g_camera.position =
        HMM_V3(start_room.center_y() * cw, 1.0f, start_room.center_x() * cw);
  } else {
    // Fallback si no hay salas (muy poco probable)
    g_camera.position = HMM_V3(5.0f, 1.0f, 5.0f);
  }
  g_camera.update_vectors();

  sapp_lock_mouse(true);

  // Cargar modelo 3D de la pared
  std::string obj_path = "assets/scifi/OBJ/Walls/WallAstra_Straight.obj";
  std::string tex_path = "assets/scifi/Textures/T_Trim_01_BaseColor.png";
  if (!load_obj(obj_path, tex_path, g_wall_mesh)) {
    printf("Error cargando pared %s\n", obj_path.c_str());
  }

  // Cargar modelo 3D del suelo
  std::string floor_obj_path =
      "assets/scifi/OBJ/Platforms/Platform_DarkPlates.obj";
  std::string floor_tex_path =
      "assets/scifi/Textures/T_Trim_01_BaseColor.png"; // Usando un trim
                                                       // genérico si no hay uno
                                                       // específico
  if (!load_obj(floor_obj_path, floor_tex_path, g_floor_mesh)) {
    printf("Error cargando suelo %s\n", floor_obj_path.c_str());
  }

  // Generar instancias de la pared y suelo según el mapa
  std::vector<HMM_Mat4> instances;
  std::vector<HMM_Mat4> floor_instances;
  int **matrix = g_config.levels[0].map->get_matrix();
  int size = g_config.levels[0].map->get_size();

  // Tamaño aproximado de la celda en 3D
  float cw = 2.0f;

  for (int z = 0; z < size; z++) {
    for (int x = 0; x < size; x++) {
      if (matrix[z][x] == 0) { // 0 es pared
        // Nueva orientación: Z (2D) representa X en el mapa 3D (yendo hacia -X)
        // X (2D) representa Z en el mapa 3D
        HMM_Mat4 model = HMM_Translate(HMM_V3(-z * cw, 0.0f, x * cw));
        instances.push_back(model);
      } else if (matrix[z][x] == 1) { // 1 es pasillo o sala
        // El suelo se construye con 2 baldosas por celda para cubrirla.
        // Asumiendo que cw es 2.0f, cada baldosa mide ~1x2 o similar.
        // Posicionamos 2 mitades. Haremos 2 baldosas adyacentes a nivel Y =
        // -0.05f
        HMM_Mat4 f_model1 =
            HMM_Translate(HMM_V3(-z * cw - 0.5f, -0.05f, x * cw));
        HMM_Mat4 f_model2 =
            HMM_Translate(HMM_V3(-z * cw + 0.5f, -0.05f, x * cw));
        floor_instances.push_back(f_model1);
        floor_instances.push_back(f_model2);
      }
    }
  }

  g_num_instances = (int)instances.size();
  g_num_floor_instances = (int)floor_instances.size();

  // Buffer de instanciación para paredes
  sg_buffer_desc inst_desc = {};
  inst_desc.usage.vertex_buffer = true;
  inst_desc.data = SG_RANGE(instances);
  g_inst_buf = sg_make_buffer(&inst_desc);

  // Buffer de instanciación para el suelo
  sg_buffer_desc floor_inst_desc = {};
  floor_inst_desc.usage.vertex_buffer = true;
  floor_inst_desc.data = SG_RANGE(floor_instances);
  g_floor_inst_buf = sg_make_buffer(&floor_inst_desc);

  // Crear Sampler genérico
  sg_sampler_desc smp_desc = {};
  smp_desc.min_filter = SG_FILTER_LINEAR;
  smp_desc.mag_filter = SG_FILTER_LINEAR;
  smp_desc.wrap_u = SG_WRAP_REPEAT;
  smp_desc.wrap_v = SG_WRAP_REPEAT;
  g_sampler = sg_make_sampler(&smp_desc);

  // Configurar Pipeline
  sg_pipeline_desc pip_desc = {};
  pip_desc.layout.buffers[0].stride = sizeof(Vertex);
  pip_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3; // pos
  pip_desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT3; // normal
  pip_desc.layout.attrs[2].format = SG_VERTEXFORMAT_FLOAT2; // uv

  pip_desc.layout.buffers[1].stride = sizeof(HMM_Mat4);
  pip_desc.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
  // Mat4 en 4 vec4 porque son variables de vértice
  pip_desc.layout.attrs[3].format = SG_VERTEXFORMAT_FLOAT4;
  pip_desc.layout.attrs[3].buffer_index = 1;
  pip_desc.layout.attrs[4].format = SG_VERTEXFORMAT_FLOAT4;
  pip_desc.layout.attrs[4].buffer_index = 1;
  pip_desc.layout.attrs[5].format = SG_VERTEXFORMAT_FLOAT4;
  pip_desc.layout.attrs[5].buffer_index = 1;
  pip_desc.layout.attrs[6].format = SG_VERTEXFORMAT_FLOAT4;
  pip_desc.layout.attrs[6].buffer_index = 1;

  pip_desc.shader = create_instanced_shader();
  pip_desc.index_type = SG_INDEXTYPE_UINT32;
  pip_desc.cull_mode = SG_CULLMODE_BACK;
  pip_desc.depth.write_enabled = true;
  pip_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;

  g_pip = sg_make_pipeline(&pip_desc);

  last_time = stm_now();
}

// Callback: se ejecuta cada frame
static void frame_cb(void) {
  float dt = (float)stm_sec(stm_diff(stm_now(), last_time));
  last_time = stm_now();

  // Movimiento de jugador: W, A, S, D en el plano ZX
  float speed = 5.0f * dt;

  // Extraer vectores directores solo en plano XZ y normalizarlos
  HMM_Vec3 move_forward = HMM_V3(g_camera.forward.X, 0.0f, g_camera.forward.Z);
  if (HMM_LenV3(move_forward) > 0.001f) {
    move_forward = HMM_NormV3(move_forward);
  }

  HMM_Vec3 move_right = HMM_V3(g_camera.right.X, 0.0f, g_camera.right.Z);
  if (HMM_LenV3(move_right) > 0.001f) {
    move_right = HMM_NormV3(move_right);
  }

  if (g_input.keys[SAPP_KEYCODE_W]) {
    g_camera.position =
        HMM_AddV3(g_camera.position, HMM_MulV3F(move_forward, speed));
  }
  if (g_input.keys[SAPP_KEYCODE_S]) {
    g_camera.position =
        HMM_SubV3(g_camera.position, HMM_MulV3F(move_forward, speed));
  }
  if (g_input.keys[SAPP_KEYCODE_A]) {
    g_camera.position =
        HMM_SubV3(g_camera.position, HMM_MulV3F(move_right, speed));
  }
  if (g_input.keys[SAPP_KEYCODE_D]) {
    g_camera.position =
        HMM_AddV3(g_camera.position, HMM_MulV3F(move_right, speed));
  }

  // Generar las matrices MVP (View y Projection)
  HMM_Mat4 view = g_camera.get_view_matrix();
  HMM_Mat4 proj = HMM_Perspective_RH_ZO(
      HMM_AngleDeg(60.0f), (float)sapp_width() / (float)sapp_height(), 0.1f,
      100.0f);

  vs_params_t vs_params;
  vs_params.view_proj = HMM_MulM4(proj, view); // M4 Multiply

  sg_pass pass = {};
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].clear_value = {0.05f, 0.05f, 0.15f, 1.0f};
  pass.action.depth.load_action = SG_LOADACTION_CLEAR;
  pass.action.depth.clear_value = 1.0f;
  pass.swapchain = sglue_swapchain();

  sg_begin_pass(&pass);

  if (g_wall_mesh.num_indices > 0 &&
      g_wall_mesh.diffuse_img.id != SG_INVALID_ID) {
    sg_apply_pipeline(g_pip);
    sg_bindings binds = {};
    binds.vertex_buffers[0] = g_wall_mesh.vbuf;
    binds.vertex_buffers[1] = g_inst_buf;
    binds.index_buffer = g_wall_mesh.ibuf;

    binds.views[0] = g_wall_mesh.diffuse_view;
    binds.samplers[0].id = g_sampler.id;
    sg_apply_bindings(&binds);

    sg_range params_range = SG_RANGE(vs_params);
    sg_apply_uniforms(0, &params_range);

    // Draw walls
    sg_draw(0, g_wall_mesh.num_indices, g_num_instances);
  }

  // Draw floors
  if (g_floor_mesh.num_indices > 0 &&
      g_floor_mesh.diffuse_img.id != SG_INVALID_ID) {
    // Reutilizamos el mismo pipeline estático pero cambiamos los bindings
    sg_apply_pipeline(g_pip);
    sg_bindings f_binds = {};
    f_binds.vertex_buffers[0] = g_floor_mesh.vbuf;
    f_binds.vertex_buffers[1] = g_floor_inst_buf;
    f_binds.index_buffer = g_floor_mesh.ibuf;

    f_binds.views[0] = g_floor_mesh.diffuse_view;
    f_binds.samplers[0].id = g_sampler.id;
    sg_apply_bindings(&f_binds);

    sg_range params_range = SG_RANGE(vs_params);
    sg_apply_uniforms(0, &params_range);

    sg_draw(0, g_floor_mesh.num_indices, g_num_floor_instances);
  }

  sg_end_pass();
  sg_commit();
}

// Callback: se ejecuta al cerrar la aplicación
static void cleanup_cb(void) {
  g_wall_mesh.destroy();
  g_floor_mesh.destroy();
  sg_destroy_buffer(g_inst_buf);
  sg_destroy_buffer(g_floor_inst_buf);
  sg_destroy_pipeline(g_pip);
  sg_destroy_sampler(g_sampler);
  sg_shutdown();
  printf("Juego finalizado.\n");
}

// Callback: se ejecuta cuando hay un evento de input
static void event_cb(const sapp_event *ev) {
  if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
    if (ev->key_code == SAPP_KEYCODE_ESCAPE) {
      sapp_request_quit();
    }
    if (ev->key_code < 512) {
      g_input.keys[ev->key_code] = true;
    }
  } else if (ev->type == SAPP_EVENTTYPE_KEY_UP) {
    if (ev->key_code < 512) {
      g_input.keys[ev->key_code] = false;
    }
  } else if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
    sapp_lock_mouse(true);
  } else if (ev->type == SAPP_EVENTTYPE_MOUSE_MOVE && sapp_mouse_locked()) {
    g_camera.yaw -= ev->mouse_dx * 0.25f;
    g_camera.pitch -= ev->mouse_dy * 0.25f;

    // Limitar el pitch para no dar la vuelta entera
    if (g_camera.pitch > 89.0f)
      g_camera.pitch = 89.0f;
    if (g_camera.pitch < -89.0f)
      g_camera.pitch = -89.0f;

    g_camera.update_vectors();
  }
}

// Punto de entrada de sokol (reemplaza a main)
sapp_desc sokol_main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  sapp_desc app = {};
  app.init_cb = init_cb;
  app.frame_cb = frame_cb;
  app.cleanup_cb = cleanup_cb;
  app.event_cb = event_cb;
  app.width = 1280;
  app.height = 720;
  app.window_title = "Proyecto Final CG - Roguelike 3D";
  app.logger.func = slog_func;

  return app;
}
