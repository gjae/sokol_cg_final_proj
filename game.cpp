#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_time.h"

#include "camera.hpp"
#include "collision_verify.hpp"
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
static sg_view g_dummy_white_view;
static sg_view g_dummy_black_view;
static sg_view g_dummy_normal_view;
static sg_view g_dummy_orm_view;
static sg_image g_dummy_white_img;
static sg_image g_dummy_black_img;
static sg_image g_dummy_normal_img;
static sg_image g_dummy_orm_img;

// Genera un quad plano de 4x4 unidades (centrado en el origen, Y=0)
static void create_floor_quad(Mesh &out_mesh, const std::string &tex_path) {
  float half = 2.0f; // 4x4 tile => half-extent = 2
  Vertex verts[4] = {
      {HMM_V3(-half, 0, -half), HMM_V3(0, 1, 0), HMM_V2(0, 0)},
      {HMM_V3(half, 0, -half), HMM_V3(0, 1, 0), HMM_V2(1, 0)},
      {HMM_V3(half, 0, half), HMM_V3(0, 1, 0), HMM_V2(1, 1)},
      {HMM_V3(-half, 0, half), HMM_V3(0, 1, 0), HMM_V2(0, 1)},
  };
  uint32_t idxs[6] = {0, 1, 2, 0, 2, 3};

  out_mesh.num_indices = 6;

  sg_buffer_desc vb = {};
  vb.usage.vertex_buffer = true;
  vb.data = {verts, sizeof(verts)};
  out_mesh.vbuf = sg_make_buffer(&vb);

  sg_buffer_desc ib = {};
  ib.usage.index_buffer = true;
  ib.data = {idxs, sizeof(idxs)};
  out_mesh.ibuf = sg_make_buffer(&ib);

  // Cargar textura difusa y todos los mapas complementarios
  load_textures(tex_path, out_mesh);
}

// Callback: se ejecuta una vez al iniciar la aplicación
static void init_cb(void) {
  sg_desc desc = {};
  desc.environment = sglue_environment();
  desc.logger.func = slog_func;
  sg_setup(&desc);

  stm_setup();

  // Crear texturas dummy para evitar pánicos de Sokol si faltan mapas
  auto create_dummy = [](uint32_t color) {
    sg_image_desc d = {};
    d.width = 1;
    d.height = 1;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.data.mip_levels[0].ptr = &color;
    d.data.mip_levels[0].size = 4;
    sg_image img = sg_make_image(&d);
    sg_view_desc vd = {};
    vd.texture.image = img;
    return std::make_pair(img, sg_make_view(&vd));
  };

  auto white = create_dummy(0xFFFFFFFF);
  g_dummy_white_img = white.first;
  g_dummy_white_view = white.second;
  auto black = create_dummy(0xFF000000);
  g_dummy_black_img = black.first;
  g_dummy_black_view = black.second;
  auto norm = create_dummy(0xFFFF8080); // Neutral Normal (128, 128, 255)
  g_dummy_normal_img = norm.first;
  g_dummy_normal_view = norm.second;
  auto orm = create_dummy(0xFF00FFFF); // AO=1, Rough=1, Metal=0 (255, 255, 0)
  g_dummy_orm_img = orm.first;
  g_dummy_orm_view = orm.second;

  // Generar niveles y colectables
  int base_size = 10;
  create_levels(g_config.levels, base_size);

  for (int i = 0; i < 3; i++) {
    g_config.collectable_count[i] = place_collectables(
        g_config.collectables[i], g_config.levels[i].map->rooms,
        g_config.levels[i].map->room_count,
        g_config.levels[i].map->get_matrix());
  }

  g_config.current_level = GameConfig::START_LEVEL;
  int lvl = g_config.current_level;

  printf("Juego inicializado - Nivel %d (mapa %dx%d) - %d colectables\n",
         g_config.levels[lvl].level, g_config.levels[lvl].map->get_size(),
         g_config.levels[lvl].map->get_size(), g_config.collectable_count[lvl]);

  // Posicionar la cámara en una sala aleatoria
  if (g_config.levels[lvl].map->room_count > 0) {
    int start_room_idx = rand() % g_config.levels[lvl].map->room_count;
    printf("Sala inicial: %d\n", start_room_idx);
    Room start_room = g_config.levels[lvl].map->rooms[start_room_idx];
    float cw = 4.0f; // Tamaño fijo de la celda de geometría 3D
    // El mapa se generó extendiéndose hacia -X y +Z.
    // center_y() corresponde a la fila (eje -X) y center_x() corresponde a la
    // columna (eje Z)
    g_camera.position =
        HMM_V3(-start_room.center_y() * cw, 1.0f, start_room.center_x() * cw);
    g_camera.yaw = 180.0f; // Mirar hacia -X

    printf("\n=== SETUP INICIAL ===\n");
    printf("Posicion de la camara: %.2f, %.2f, %.2f\n", g_camera.position.X,
           g_camera.position.Y, g_camera.position.Z);
    printf("Centro de sala (y, x) 2D: %d, %d\n", start_room.center_y(),
           start_room.center_x());
  } else {
    // Fallback si no hay salas
    g_camera.position = HMM_V3(-5.0f, 1.0f, 5.0f);
    g_camera.yaw = 180.0f;
  }
  g_camera.update_vectors();

  sapp_lock_mouse(true);

  // Cargar modelo 3D de la pared
  std::string obj_path = "assets/scifi/OBJ/Walls/WallAstra_Straight.obj";
  std::string tex_path = "assets/scifi/Textures/T_Trim_01_BaseColor.png";
  if (!load_obj(obj_path, tex_path, g_wall_mesh)) {
    printf("Error cargando pared %s\n", obj_path.c_str());
  }

  // Generar quad plano para el suelo (evita discontinuidades del OBJ)
  std::string floor_tex_path = "assets/scifi/Textures/T_Trim_01_BaseColor.png";
  create_floor_quad(g_floor_mesh, floor_tex_path);

  // Generar instancias de la pared y suelo según el mapa
  std::vector<HMM_Mat4> instances;
  std::vector<HMM_Mat4> floor_instances;
  int **matrix = g_config.levels[lvl].map->get_matrix();
  int size = g_config.levels[lvl].map->get_size();

  // Las baldosas miden exactamente 4.0f x 4.0f unidades en el archivo OBJ.
  // Ajustar la distancia entre celdas del mapa (cw) para que coincida
  // exactamente y no haya huecos.
  float cw = 4.0f;
  float half_cw = cw * 0.5f;

  for (int z = 0; z < size; z++) {
    for (int x = 0; x < size; x++) {
      if (matrix[z][x] != 0) { // Cualquier valor != 0 es espacio transitable
        float cx = -z * cw;
        float cz = x * cw;

        HMM_Mat4 tf = HMM_Translate(HMM_V3(cx, 0.0f, cz));

        // Instanciar suelo (quad plano, sin escala extra)
        floor_instances.push_back(tf);

        // La pared "Astra_Straight" NO está centrada. Su cara interna está en
        // X=-1.56. El borde del suelo está en X=-2.0. Aplicamos un offset de
        // -0.44 para alinear.
        HMM_Mat4 wall_offset = HMM_Translate(HMM_V3(-0.44f, 0.0f, 0.0f));

        // -X edge in 3D (z+1 in map)
        if (z == size - 1 || matrix[z + 1][x] == 0) {
          instances.push_back(HMM_MulM4(tf, wall_offset));
        }
        // +X edge in 3D (z-1 in map)
        if (z == 0 || matrix[z - 1][x] == 0) {
          HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(180.0f), HMM_V3(0, 1, 0));
          instances.push_back(HMM_MulM4(tf, HMM_MulM4(rot, wall_offset)));
        }
        // -Z edge in 3D (x-1 in map)
        if (x == 0 || matrix[z][x - 1] == 0) {
          HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(-90.0f), HMM_V3(0, 1, 0));
          instances.push_back(HMM_MulM4(tf, HMM_MulM4(rot, wall_offset)));
        }
        // +Z edge in 3D (x+1 in map)
        if (x == size - 1 || matrix[z][x + 1] == 0) {
          HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(90.0f), HMM_V3(0, 1, 0));
          instances.push_back(HMM_MulM4(tf, HMM_MulM4(rot, wall_offset)));
        }
      }
    }
  }

  // Fallback to avoid Sokol crashing on size=0 buffers from empty procedural
  // maps
  if (instances.empty()) {
    instances.push_back(HMM_Scale(HMM_V3(0.0f, 0.0f, 0.0f)));
  }
  if (floor_instances.empty()) {
    floor_instances.push_back(HMM_Scale(HMM_V3(0.0f, 0.0f, 0.0f)));
  }

  g_num_instances = (int)instances.size();
  g_num_floor_instances = (int)floor_instances.size();

  // Buffer de instanciación para paredes
  sg_buffer_desc inst_desc = {};
  inst_desc.usage.vertex_buffer = true;
  inst_desc.data = {instances.data(), instances.size() * sizeof(HMM_Mat4)};
  g_inst_buf = sg_make_buffer(&inst_desc);

  // Buffer de instanciación para el suelo
  sg_buffer_desc floor_inst_desc = {};
  floor_inst_desc.usage.vertex_buffer = true;
  floor_inst_desc.data = {floor_instances.data(),
                          floor_instances.size() * sizeof(HMM_Mat4)};
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
  pip_desc.layout.attrs[3].offset = 0;
  pip_desc.layout.attrs[4].format = SG_VERTEXFORMAT_FLOAT4;
  pip_desc.layout.attrs[4].buffer_index = 1;
  pip_desc.layout.attrs[4].offset = 16;
  pip_desc.layout.attrs[5].format = SG_VERTEXFORMAT_FLOAT4;
  pip_desc.layout.attrs[5].buffer_index = 1;
  pip_desc.layout.attrs[5].offset = 32;
  pip_desc.layout.attrs[6].format = SG_VERTEXFORMAT_FLOAT4;
  pip_desc.layout.attrs[6].buffer_index = 1;
  pip_desc.layout.attrs[6].offset = 48;

  pip_desc.shader = create_instanced_shader();
  pip_desc.index_type = SG_INDEXTYPE_UINT32;
  pip_desc.cull_mode =
      SG_CULLMODE_NONE; // Disable culling in case floor normals are inverted
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
  float speed = 20.0f * dt;

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

  // --- Collision Detection & Resolution ---
  g_camera.collider.center = HMM_V2(g_camera.position.X, g_camera.position.Z);

  // Determinar celda actual
  float cw = 4.0f;
  float half_cw = cw * 0.5f;
  int current_z = (int)round(-g_camera.position.X / cw);
  int current_x = (int)round(g_camera.position.Z / cw);

  int **matrix = g_config.levels[g_config.current_level].map->get_matrix();
  int size = g_config.levels[g_config.current_level].map->get_size();

  // Revisar celdas vecinas en un radio de 1 y resolver colisiones con paredes
  // (valor 0)
  for (int dz = -1; dz <= 1; dz++) {
    for (int dx = -1; dx <= 1; dx++) {
      int check_z = current_z + dz;
      int check_x = current_x + dx;

      // Si la celda está fuera de los límites o es pared (0)
      if (check_z < 0 || check_z >= size || check_x < 0 || check_x >= size ||
          matrix[check_z][check_x] == 0) {

        float wall_cx = -check_z * cw;
        float wall_cz = check_x * cw;

        AABB wall_box;
        wall_box.min = HMM_V2(wall_cx - half_cw, wall_cz - half_cw);
        wall_box.max = HMM_V2(wall_cx + half_cw, wall_cz + half_cw);

        HMM_Vec2 mtv = {0};
        if (check_circle_aabb_collision(g_camera.collider, wall_box, &mtv)) {
          // Aplicar el MTV a la posición y actualizar el centro del collider
          g_camera.position.X += mtv.X;
          g_camera.position.Z += mtv.Y;
          g_camera.collider.center =
              HMM_V2(g_camera.position.X, g_camera.position.Z);
        }
      }
    }
  }
  // ----------------------------------------

  // Generar las matrices MVP (View y Projection)
  HMM_Mat4 view = g_camera.get_view_matrix();
  // Sokol GFX con backend OpenGL espera Z en rango [-1, 1] habitualmente.
  // Utilizaremos _NO (Negative One to One)
  HMM_Mat4 proj = HMM_Perspective_RH_NO(
      HMM_AngleDeg(60.0f), (float)sapp_width() / (float)sapp_height(), 0.1f,
      100.0f);

  vs_params_t vs_params;
  vs_params.view = view;
  vs_params.proj = proj;

  fs_params_t fs_params;
  fs_params.light_dir = HMM_V3(0.3f, -1.0f, 0.2f);

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

    binds.views[0] = (g_wall_mesh.diffuse_view.id != SG_INVALID_ID)
                         ? g_wall_mesh.diffuse_view
                         : g_dummy_white_view;
    binds.views[1] = (g_wall_mesh.normal_view.id != SG_INVALID_ID)
                         ? g_wall_mesh.normal_view
                         : g_dummy_normal_view;
    binds.views[2] = (g_wall_mesh.orm_view.id != SG_INVALID_ID)
                         ? g_wall_mesh.orm_view
                         : g_dummy_orm_view;
    binds.views[3] = (g_wall_mesh.emissive_view.id != SG_INVALID_ID)
                         ? g_wall_mesh.emissive_view
                         : g_dummy_black_view;
    binds.samplers[0].id = g_sampler.id;
    sg_apply_bindings(&binds);

    sg_range vs_range = SG_RANGE(vs_params);
    sg_apply_uniforms(0, &vs_range);

    sg_range fs_range = SG_RANGE(fs_params);
    sg_apply_uniforms(1, &fs_range);

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

    f_binds.views[0] = (g_floor_mesh.diffuse_view.id != SG_INVALID_ID)
                           ? g_floor_mesh.diffuse_view
                           : g_dummy_white_view;
    f_binds.views[1] = (g_floor_mesh.normal_view.id != SG_INVALID_ID)
                           ? g_floor_mesh.normal_view
                           : g_dummy_normal_view;
    f_binds.views[2] = (g_floor_mesh.orm_view.id != SG_INVALID_ID)
                           ? g_floor_mesh.orm_view
                           : g_dummy_orm_view;
    f_binds.views[3] = (g_floor_mesh.emissive_view.id != SG_INVALID_ID)
                           ? g_floor_mesh.emissive_view
                           : g_dummy_black_view;
    f_binds.samplers[0].id = g_sampler.id;
    sg_apply_bindings(&f_binds);

    sg_range vs_range = SG_RANGE(vs_params);
    sg_apply_uniforms(0, &vs_range);

    sg_range fs_range = SG_RANGE(fs_params);
    sg_apply_uniforms(1, &fs_range);

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
  sg_destroy_image(g_dummy_white_img);
  sg_destroy_image(g_dummy_black_img);
  sg_destroy_image(g_dummy_normal_img);
  sg_destroy_image(g_dummy_orm_img);
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
  } else if (ev->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
    if (!sapp_mouse_locked()) {
      // Opcional: auto-lock si se mueve el ratón en la ventana focalizada
      // sapp_lock_mouse(true);
    }
    g_camera.yaw += ev->mouse_dx * 0.25f;
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
