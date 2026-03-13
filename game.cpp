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
#include <string>
#include <vector>

#include <map>

// Estado global del juego
static GameConfig g_config;
static Camera g_camera;
static uint64_t last_time = 0;

struct InputState {
  bool keys[512]; // Keep track of pressed keys
};
static InputState g_input = {};
static bool g_show_minimap_aliens = false; // Flag for toggling minimap aliens
static HMM_Mat4 view_mat;
static HMM_Mat4 proj_mat;

// Recursos de Sokol
static Mesh g_wall_mesh;
static Mesh g_floor_mesh;
// Meshes for alien collectables
static Mesh g_alien_cyclop_mesh;
static Mesh g_alien_scolitex_mesh;
static Mesh g_alien_oculi_mesh;

static sg_pipeline g_pip;
static sg_buffer g_inst_buf;
static int g_num_instances = 0;
// Buffers unificados para aliens
static sg_buffer g_alien_cyclop_inst_buf = {SG_INVALID_ID};
static sg_buffer g_alien_scolitex_inst_buf = {SG_INVALID_ID};
static sg_buffer g_alien_oculi_inst_buf = {SG_INVALID_ID};

static bool g_stairs_active = false;
static HMM_Vec3 g_stairs_pos;

// --- Fade transition state ---
enum FadeState { FADE_NONE, FADE_OUT, FADE_IN };
static FadeState g_fade_state = FADE_NONE;
static float g_fade_alpha = 0.0f;
static const float FADE_SPEED = 2.0f; // 0.5 seconds per phase
static sg_buffer g_fade_vbuf = {
    SG_INVALID_ID};             // Buffer dedicado para el overlay fade
static bool g_game_won = false; // Pantalla de victoria

// Puerta (quad texturizado en el hueco de la pared)
static Mesh g_door_mesh;
static sg_buffer g_door_inst_buf = {SG_INVALID_ID};
static HMM_Mat4 g_door_transform;
static bool g_door_needs_update =
    false; // Flag para actualizar buffer en frame_cb
static bool g_restart_pending = false; // Flag para diferir reinicio al frame_cb

static int g_cyclop_count = 0;
static int g_scolitex_count = 0;
static int g_oculi_count = 0;

static int g_collected_count = 0; // Total aliens capturados en el nivel actual
static int g_total_aliens = 0;    // Total aliens en el nivel actual

static sg_sampler g_sampler;
static sg_view g_dummy_white_view;
static sg_view g_dummy_black_view;
static sg_view g_dummy_normal_view;
static sg_view g_dummy_orm_view;
static sg_image g_dummy_white_img;
static sg_image g_dummy_black_img;
static sg_image g_dummy_normal_img;
static sg_image g_dummy_orm_img;

// Datos para el minimapa 2D
struct MinimapVertex {
  float x, y;       // posición en NDC
  float r, g, b, a; // color
};

static sg_buffer g_minimap_vbuf;
static sg_pipeline g_minimap_pip;
static sg_shader g_minimap_shader;
static const int MAX_MINIMAP_VERTS = 6 * 64 * 64; // 6 vértices por celda

struct TextureGroup {
  sg_image diffuse_img;
  sg_view diffuse_view;
  sg_image normal_img;
  sg_view normal_view;
  sg_image orm_img;
  sg_view orm_view;
  sg_image emissive_img;
  sg_view emissive_view;
  std::vector<HMM_Mat4> instances;
  sg_buffer inst_buf;
  int num_instances = 0;

  void destroy() {
    if (diffuse_img.id != SG_INVALID_ID)
      sg_destroy_image(diffuse_img);
    if (normal_img.id != SG_INVALID_ID)
      sg_destroy_image(normal_img);
    if (orm_img.id != SG_INVALID_ID)
      sg_destroy_image(orm_img);
    if (emissive_img.id != SG_INVALID_ID)
      sg_destroy_image(emissive_img);
    if (inst_buf.id != SG_INVALID_ID)
      sg_destroy_buffer(inst_buf);
  }
};

static std::map<std::string, TextureGroup> g_floor_groups;

// Genera un quad plano de 4x4 unidades (centrado en el origen, ligeramente por
// debajo de Y=0 para solaparse con la base de las paredes)
static void create_floor_quad(Mesh &out_mesh, const std::string &tex_path) {
  float half = 2.0f; // 4x4 tile => half-extent = 2
  float y = -0.02f;  // pequeño solapado bajo las paredes
  Vertex verts[4] = {
      {HMM_V3(-half, y, -half), HMM_V3(0, 1, 0), HMM_V2(0, 0)},
      {HMM_V3(half, y, -half), HMM_V3(0, 1, 0), HMM_V2(1, 0)},
      {HMM_V3(half, y, half), HMM_V3(0, 1, 0), HMM_V2(1, 1)},
      {HMM_V3(-half, y, half), HMM_V3(0, 1, 0), HMM_V2(0, 1)},
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

static HMM_Vec2 world_to_map(HMM_Vec3 pos, float cw) {
  // Conversión inversa: cx = -(y+0.5)*cw  ==> y = (-cx/cw)-0.5
  // cz = (x+0.5)*cw ==> x = (cz/cw)-0.5
  float grid_y = (-pos.X / cw) - 0.5f;
  float grid_x = (pos.Z / cw) - 0.5f;
  return HMM_V2(grid_x, grid_y); // Returns (col, row)
}

static void build_level_geometry(int lvl) {
  // Limpiar geometrías previas
  if (g_inst_buf.id != SG_INVALID_ID) {
    sg_destroy_buffer(g_inst_buf);
    g_inst_buf.id = SG_INVALID_ID;
  }
  // Nota: NO destruimos los buffers dinámicos de aliens aquí.
  // Son buffers dinámicos gestionados por frame_cb / init_cb.

  for (auto &it : g_floor_groups) {
    if (it.second.inst_buf.id != SG_INVALID_ID) {
      sg_destroy_buffer(it.second.inst_buf);
      it.second.inst_buf.id = SG_INVALID_ID;
    }
    it.second.instances.clear();
    it.second.num_instances = 0;
  }

  std::vector<HMM_Mat4> wall_instances;
  int **matrix = g_config.levels[lvl].map->get_matrix();
  int size = g_config.levels[lvl].map->get_size();
  float cw = 4.0f;

  std::vector<std::vector<bool>> visited(size, std::vector<bool>(size, false));

  // Primero: Procesar todas las áreas (salas y pasillos) para el suelo
  for (int i = 0; i < g_config.levels[lvl].map->room_count; i++) {
    Room &r = g_config.levels[lvl].map->rooms[i];

    char id_str[4];
    snprintf(id_str, sizeof(id_str), "%02d", r.texture.id);
    std::string tex_key =
        r.texture.base + "/" + r.texture.base + "_" + id_str + "-512x512.png";
    std::string tex_path = "assets/textures_id_bind/" + tex_key;

    if (g_floor_groups.find(tex_key) == g_floor_groups.end()) {
      TextureGroup tg;
      tg.diffuse_img = g_dummy_white_img;
      tg.diffuse_view = g_dummy_white_view;
      tg.normal_img = g_dummy_normal_img;
      tg.normal_view = g_dummy_normal_view;
      tg.orm_img = g_dummy_orm_img;
      tg.orm_view = g_dummy_orm_view;
      tg.emissive_img = g_dummy_black_img;
      tg.emissive_view = g_dummy_black_view;

      Mesh dummy_mesh = {};
      load_textures(tex_path, dummy_mesh);
      if (dummy_mesh.diffuse_img.id != SG_INVALID_ID) {
        tg.diffuse_img = dummy_mesh.diffuse_img;
        tg.diffuse_view = dummy_mesh.diffuse_view;
        if (dummy_mesh.normal_view.id != SG_INVALID_ID) {
          tg.normal_img = dummy_mesh.normal_img;
          tg.normal_view = dummy_mesh.normal_view;
        }
        if (dummy_mesh.orm_view.id != SG_INVALID_ID) {
          tg.orm_img = dummy_mesh.orm_img;
          tg.orm_view = dummy_mesh.orm_view;
        }
        if (dummy_mesh.emissive_view.id != SG_INVALID_ID) {
          tg.emissive_img = dummy_mesh.emissive_img;
          tg.emissive_view = dummy_mesh.emissive_view;
        }
      }
      g_floor_groups[tex_key] = tg;
    }

    TextureGroup &group = g_floor_groups[tex_key];
    for (int ry = r.y; ry < r.y + r.h; ry++) {
      for (int rx = r.x; rx < r.x + r.w; rx++) {
        if (ry >= 0 && ry < size && rx >= 0 && rx < size) {
          if (!visited[ry][rx]) {
            visited[ry][rx] = true;
            HMM_Mat4 tf = HMM_Translate(HMM_V3(-ry * cw, 0.0f, rx * cw));
            group.instances.push_back(tf);
          }
          static const int dy[4] = {-1, 1, 0, 0};
          static const int dx[4] = {0, 0, -1, 1};
          for (int d = 0; d < 4; d++) {
            int ny = ry + dy[d];
            int nx = rx + dx[d];
            if (ny >= 0 && ny < size && nx >= 0 && nx < size) {
              if (matrix[ny][nx] == 0 && !visited[ny][nx]) {
                visited[ny][nx] = true;
                HMM_Mat4 tf_wall_floor =
                    HMM_Translate(HMM_V3(-ny * cw, 0.0f, nx * cw));
                group.instances.push_back(tf_wall_floor);
              }
            }
          }
        }
      }
    }
  }

  // Segundo: Paredes y Esquinas
  const HMM_Mat4 wall_offset = HMM_Translate(HMM_V3(-0.44f, 0.0f, 0.0f));
  for (int z = 0; z < size; z++) {
    for (int x = 0; x < size; x++) {
      if (matrix[z][x] != 0 &&
          matrix[z][x] != 99) { // Es navegable (espacio abierto)
        float cx = -z * cw;
        float cz = x * cw;
        HMM_Mat4 tf = HMM_Translate(HMM_V3(cx, -0.02f, cz));

        // Colocar muros: se pone pared si el vecino es 0 (muro) O 99 (puerta).
        // Para 99, ADEMÁS se superpone la textura de puerta encima del muro.
        auto is_wall = [&](int gz, int gx) -> bool {
          if (gz < 0 || gz >= size || gx < 0 || gx >= size)
            return true;
          return matrix[gz][gx] == 0 || matrix[gz][gx] == 99;
        };

        // Sur (z+1)
        if (is_wall(z + 1, x)) {
          wall_instances.push_back(HMM_MulM4(tf, wall_offset));
          if (z < size - 1 && matrix[z + 1][x] == 99) {
            // Puerta superpuesta: ligeramente delante del muro (+0.4 hacia el
            // room = +X)
            HMM_Mat4 fmove =
                HMM_Translate(HMM_V3(-(z + 0.5f) * cw + 0.4f, 0.0f, x * cw));
            HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(90.0f), HMM_V3(0, 1, 0));
            g_door_transform = HMM_MulM4(fmove, rot);
            g_door_needs_update = true;
          }
        }
        // Norte (z-1)
        if (is_wall(z - 1, x)) {
          HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(180.0f), HMM_V3(0, 1, 0));
          wall_instances.push_back(HMM_MulM4(tf, HMM_MulM4(rot, wall_offset)));
          if (z > 0 && matrix[z - 1][x] == 99) {
            HMM_Mat4 fmove =
                HMM_Translate(HMM_V3(-(z - 0.5f) * cw - 0.4f, 0.0f, x * cw));
            HMM_Mat4 rr = HMM_Rotate_RH(HMM_AngleDeg(-90.0f), HMM_V3(0, 1, 0));
            g_door_transform = HMM_MulM4(fmove, rr);
            g_door_needs_update = true;
          }
        }
        // Oeste (x-1)
        if (is_wall(z, x - 1)) {
          HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(-90.0f), HMM_V3(0, 1, 0));
          wall_instances.push_back(HMM_MulM4(tf, HMM_MulM4(rot, wall_offset)));
          if (x > 0 && matrix[z][x - 1] == 99) {
            HMM_Mat4 fmove =
                HMM_Translate(HMM_V3(-z * cw, 0.0f, (x - 0.5f) * cw + 0.4f));
            g_door_transform = fmove;
            g_door_needs_update = true;
          }
        }
        // Este (x+1)
        if (is_wall(z, x + 1)) {
          HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(90.0f), HMM_V3(0, 1, 0));
          wall_instances.push_back(HMM_MulM4(tf, HMM_MulM4(rot, wall_offset)));
          if (x < size - 1 && matrix[z][x + 1] == 99) {
            HMM_Mat4 fmove =
                HMM_Translate(HMM_V3(-z * cw, 0.0f, (x + 0.5f) * cw - 0.4f));
            HMM_Mat4 rr = HMM_Rotate_RH(HMM_AngleDeg(180.0f), HMM_V3(0, 1, 0));
            g_door_transform = HMM_MulM4(fmove, rr);
            g_door_needs_update = true;
          }
        }
      } else if (matrix[z][x] == 99) { // Es puerta
        g_stairs_active = true;
        g_stairs_pos = HMM_V3(-(z + 0.5f) * cw, 0.0f, (x + 0.5f) * cw);
      }
    }
  }

  for (int z = 0; z < size; z++) {
    for (int x = 0; x < size; x++) {
      if (matrix[z][x] == 0 || matrix[z][x] == 99)
        continue; // Pared o escalera

      int north = (z > 0) ? matrix[z - 1][x] : 0;
      int south = (z < size - 1) ? matrix[z + 1][x] : 0;
      int west = (x > 0) ? matrix[z][x - 1] : 0;
      int east = (x < size - 1) ? matrix[z][x + 1] : 0;

      float cx_corner = -z * cw;
      float cz_corner = x * cw;
      float c_off = 0.4595f;

      if (north == 0 && east == 0) {
        float cx = cx_corner + c_off;
        float cz = cz_corner + c_off;
        HMM_Mat4 tf_corner = HMM_Translate(HMM_V3(cx, -0.02f, cz));
        HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(135.0f), HMM_V3(0, 1, 0));
        wall_instances.push_back(
            HMM_MulM4(tf_corner, HMM_MulM4(rot, wall_offset)));
      }
      if (north == 0 && west == 0) {
        float cx = cx_corner + c_off;
        float cz = cz_corner - c_off;
        HMM_Mat4 tf_corner = HMM_Translate(HMM_V3(cx, -0.02f, cz));
        HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(-135.0f), HMM_V3(0, 1, 0));
        wall_instances.push_back(
            HMM_MulM4(tf_corner, HMM_MulM4(rot, wall_offset)));
      }
      if (south == 0 && east == 0) {
        float cx = cx_corner - c_off;
        float cz = cz_corner + c_off;
        HMM_Mat4 tf_corner = HMM_Translate(HMM_V3(cx, -0.02f, cz));
        HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(45.0f), HMM_V3(0, 1, 0));
        wall_instances.push_back(
            HMM_MulM4(tf_corner, HMM_MulM4(rot, wall_offset)));
      }
      if (south == 0 && west == 0) {
        float cx = cx_corner - c_off;
        float cz = cz_corner - c_off;
        HMM_Mat4 tf_corner = HMM_Translate(HMM_V3(cx, -0.02f, cz));
        HMM_Mat4 rot = HMM_Rotate_RH(HMM_AngleDeg(-45.0f), HMM_V3(0, 1, 0));
        wall_instances.push_back(
            HMM_MulM4(tf_corner, HMM_MulM4(rot, wall_offset)));
      }
    }
  }

  if (wall_instances.empty()) {
    wall_instances.push_back(HMM_Scale(HMM_V3(0.0f, 0.0f, 0.0f)));
  }

  g_num_instances = (int)wall_instances.size();
  sg_buffer_desc inst_desc = {};
  inst_desc.usage.vertex_buffer = true;
  inst_desc.data = {wall_instances.data(),
                    wall_instances.size() * sizeof(HMM_Mat4)};
  g_inst_buf = sg_make_buffer(&inst_desc);

  for (auto &it : g_floor_groups) {
    if (it.second.instances.empty())
      continue;
    it.second.num_instances = (int)it.second.instances.size();
    sg_buffer_desc f_inst_desc = {};
    f_inst_desc.usage.vertex_buffer = true;
    f_inst_desc.data = {it.second.instances.data(),
                        it.second.instances.size() * sizeof(HMM_Mat4)};
    it.second.inst_buf = sg_make_buffer(&f_inst_desc);
  }

  // --- Parsear items collectables y crear sus buffers instanciados ---
  std::vector<HMM_Mat4> cyclop_inst;
  std::vector<HMM_Mat4> scolitex_inst;
  std::vector<HMM_Mat4> oculi_inst;
  g_total_aliens = g_config.collectable_count[lvl];

  // Guardar el valor real de g_collected_count para no reiniciarlo
  int real_collected_count = g_collected_count;
  g_collected_count = 0;

  for (int i = 0; i < g_total_aliens; i++) {
    Collectable &c = g_config.collectables[lvl][i];

    // Si la textura (alien ID) es -1, significa que ya fue capturado y no se
    // dibuja ni calcula
    if (c.texture_id == -1)
      continue;

    // La matriz del mapa se lee con índices z=fila, x=col.
    // En el mundo 3D: el eje X va de positivo a negativo (filas z)
    // y el eje Z va de negativo a positivo (columnas x).
    // Las salas colocan paredes centradas, así que los aliens también van en el
    // centro de su baldosa
    float cx = -(c.y + 0.5f) * cw;
    float cz = (c.x + 0.5f) * cw;
    HMM_Vec3 alien_pos = HMM_V3(cx, 0.0f, cz);

    // Check de colisión por AABB (Axis-Aligned Bounding Box)
    // Asumimos un radio/ancho y profundidad aproximado para el alien escalado
    // (ej: 0.8 unids)
    float aabb_hx = 0.8f;
    float aabb_hz = 0.8f;
    // Y la cámara representa al jugador con cierto grosor (ej: 0.3 unids)
    float cam_hx = 0.3f;
    float cam_hz = 0.3f;

    // Verificamos solapamiento en el eje X y Z
    bool overlap_x = (g_camera.position.X + cam_hx >= alien_pos.X - aabb_hx) &&
                     (g_camera.position.X - cam_hx <= alien_pos.X + aabb_hx);
    bool overlap_z = (g_camera.position.Z + cam_hz >= alien_pos.Z - aabb_hz) &&
                     (g_camera.position.Z - cam_hz <= alien_pos.Z + aabb_hz);

    if (overlap_x && overlap_z) {
      c.texture_id = -1; // Marcar como recolectado
      g_collected_count++;
      continue;
    }

    // Escala menor (0.25f) para que no sean gigantes y obstaculicen las paredes
    HMM_Mat4 scale = HMM_Scale(HMM_V3(0.25f, 0.25f, 0.25f));
    HMM_Mat4 move = HMM_Translate(alien_pos);
    HMM_Mat4 rot =
        HMM_Rotate_RH(HMM_AngleDeg(c.x * c.y * 30.0f),
                      HMM_V3(0, 1, 0)); // Rotación pseudoaleatoria estable
    HMM_Mat4 tf = HMM_MulM4(move, HMM_MulM4(rot, scale));

    if (c.texture_id == 100)
      cyclop_inst.push_back(tf);
    else if (c.texture_id == 101)
      scolitex_inst.push_back(tf);
    else
      oculi_inst.push_back(tf);
  }

  auto update_dynamic_buf = [](sg_buffer buf,
                               const std::vector<HMM_Mat4> &inst) {
    if (!inst.empty()) {
      sg_update_buffer(buf, {inst.data(), inst.size() * sizeof(HMM_Mat4)});
    }
  };

  g_cyclop_count = cyclop_inst.size();
  g_scolitex_count = scolitex_inst.size();
  g_oculi_count = oculi_inst.size();
  update_dynamic_buf(g_alien_cyclop_inst_buf, cyclop_inst);
  update_dynamic_buf(g_alien_scolitex_inst_buf, scolitex_inst);
  update_dynamic_buf(g_alien_oculi_inst_buf, oculi_inst);

  // Restaurar el valor real
  g_collected_count = real_collected_count;
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

  // Cargar modelos 3D de los aliens (collectables)
  // Nota: Las texturas pueden no existir como basecolor per-se, usamos blanca o
  // por defecto.
  if (!load_obj("assets/scifi/OBJ/Aliens/Alien_Cyclop.obj", "",
                g_alien_cyclop_mesh)) {
    printf("Error cargando Alien_Cyclop\n");
  }
  if (!load_obj("assets/scifi/OBJ/Aliens/Alien_Scolitex.obj", "",
                g_alien_scolitex_mesh)) {
    printf("Error cargando Alien_Scolitex\n");
  }
  if (!load_obj("assets/scifi/OBJ/Aliens/Alien_Oculichrysalis.obj", "",
                g_alien_oculi_mesh)) {
    printf("Error cargando Alien_Oculichrysalis\n");
  }

  create_floor_quad(g_floor_mesh, "");

  // Crear quad para la puerta (un plano vertical de 4x4 con textura)
  {
    float half = 2.0f;
    // UVs mapeados para que la textura no se repita y se vea de frente.
    // Asumiendo que bottom-left es (0,1) o (0,0) dependiendo de la carga de
    // imagen. Sokol gfx con stb_image suele tener (0,0) top-left.
    Vertex door_verts[4] = {
        {HMM_V3(-half, 0, 0), HMM_V3(0, 0, 1), HMM_V2(0, 1)}, // Bottom-left
        {HMM_V3(half, 0, 0), HMM_V3(0, 0, 1), HMM_V2(1, 1)},  // Bottom-right
        {HMM_V3(half, half * 2, 0), HMM_V3(0, 0, 1), HMM_V2(1, 0)}, // Top-right
        {HMM_V3(-half, half * 2, 0), HMM_V3(0, 0, 1), HMM_V2(0, 0)}, // Top-left
    };
    uint32_t door_idxs[6] = {0, 1, 2, 0, 2, 3};
    g_door_mesh.num_indices = 6;
    sg_buffer_desc vb = {};
    vb.usage.vertex_buffer = true;
    vb.data = {door_verts, sizeof(door_verts)};
    g_door_mesh.vbuf = sg_make_buffer(&vb);
    sg_buffer_desc ib = {};
    ib.usage.index_buffer = true;
    ib.data = {door_idxs, sizeof(door_idxs)};
    g_door_mesh.ibuf = sg_make_buffer(&ib);
    load_textures("assets/doors/door1.png", g_door_mesh);
  }
  // Buffer de instancia para la puerta (1 sola instancia, dinámico)
  {
    sg_buffer_desc dd = {};
    dd.usage.vertex_buffer = true;
    dd.usage.dynamic_update = true;
    dd.size = sizeof(HMM_Mat4);
    g_door_inst_buf = sg_make_buffer(&dd);
  }

  // Construir geometría modular (suelos y paredes)
  build_level_geometry(lvl);

  // --- Inicializar Buffers Dinámicos para Aliens ---
  auto make_dynamic_buf = [](int max_elements) -> sg_buffer {
    sg_buffer_desc desc = {};
    desc.usage.vertex_buffer = true;
    desc.usage.dynamic_update = true;
    desc.size = max_elements * sizeof(HMM_Mat4);
    return sg_make_buffer(&desc);
  };
  g_alien_cyclop_inst_buf = make_dynamic_buf(30);
  g_alien_scolitex_inst_buf = make_dynamic_buf(30);
  g_alien_oculi_inst_buf = make_dynamic_buf(30);

  // Buffer dedicado para el overlay de fade (6 vértices)
  {
    sg_buffer_desc fd = {};
    fd.usage.vertex_buffer = true;
    fd.usage.dynamic_update = true;
    fd.size = 6 * sizeof(MinimapVertex);
    g_fade_vbuf = sg_make_buffer(&fd);
  }

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

  // --- Pipeline y recursos para el minimapa 2D ---
  const char *mm_vs_src = R"(
#version 330
layout(location=0) in vec2 pos;
layout(location=1) in vec4 color;
out vec4 v_color;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    v_color = color;
}
)";

  const char *mm_fs_src = R"(
#version 330
in vec4 v_color;
out vec4 frag_color;
void main() {
    frag_color = v_color;
}
)";

  sg_shader_desc mm_shd_desc = {};
  mm_shd_desc.vertex_func.source = mm_vs_src;
  mm_shd_desc.fragment_func.source = mm_fs_src;
  g_minimap_shader = sg_make_shader(&mm_shd_desc);

  sg_pipeline_desc mm_pip_desc = {};
  mm_pip_desc.shader = g_minimap_shader;
  mm_pip_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2; // pos
  mm_pip_desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4; // color
  mm_pip_desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
  mm_pip_desc.depth.write_enabled = false;
  mm_pip_desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
  mm_pip_desc.colors[0].blend.enabled = true;
  mm_pip_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
  mm_pip_desc.colors[0].blend.dst_factor_rgb =
      SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

  g_minimap_pip = sg_make_pipeline(&mm_pip_desc);

  sg_buffer_desc mm_buf_desc = {};
  // Buffer de vértices actualizable en cada frame
  mm_buf_desc.size = sizeof(MinimapVertex) * MAX_MINIMAP_VERTS;
  mm_buf_desc.usage.vertex_buffer = true;
  mm_buf_desc.usage.dynamic_update = true;
  // Para buffers dinámicos no se debe pasar data inicial
  mm_buf_desc.data.ptr = nullptr;
  mm_buf_desc.data.size = 0;
  g_minimap_vbuf = sg_make_buffer(&mm_buf_desc);

  last_time = stm_now();
}

// Callback: se ejecuta cada frame
static void frame_cb(void) {
  float dt = (float)stm_sec(stm_diff(stm_now(), last_time));
  last_time = stm_now();

  // --- Manejar reinicio pendiente (diferido desde event_cb) ---
  if (g_restart_pending) {
    g_restart_pending = false;
    g_game_won = false;
    g_config.current_level = GameConfig::START_LEVEL;
    g_stairs_active = false;
    g_total_aliens = g_config.collectable_count[g_config.current_level];
    g_collected_count = 0;
    g_fade_state = FADE_NONE;
    g_fade_alpha = 0.0f;
    g_door_needs_update = false;

    for (int l = 0; l < 3; l++) {
      for (int i = 0; i < g_config.collectable_count[l]; i++) {
        g_config.collectables[l][i].texture_id = 100 + (i % 3);
      }
    }

    build_level_geometry(g_config.current_level);

    float cw = 4.0f;
    int lvl_r = g_config.current_level;
    int start_room_idx = rand() % g_config.levels[lvl_r].map->room_count;
    Room start_room = g_config.levels[lvl_r].map->rooms[start_room_idx];
    g_camera.position =
        HMM_V3(-start_room.center_y() * cw, 1.0f, start_room.center_x() * cw);
    g_camera.yaw = 180.0f;
    g_camera.update_vectors();
    sapp_lock_mouse(true);

    return; // Evita actualizar los buffers otra vez este frame (Sokol error:
            // only one update allowed per buffer and frame)
  }

  // Movimiento de jugador: solo si no hay fade transition
  if (g_fade_state == FADE_NONE) {
    float speed = 20.0f * dt;

    // Extraer vectores directores solo en plano XZ y normalizarlos
    HMM_Vec3 move_forward =
        HMM_V3(g_camera.forward.X, 0.0f, g_camera.forward.Z);
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

  if (!g_game_won) {
    // --- AABB Collectables & Stairs Logic ---
    float cw_3d = 4.0f;
    std::vector<HMM_Mat4> cyclop_inst;
    std::vector<HMM_Mat4> scolitex_inst;
    std::vector<HMM_Mat4> oculi_inst;
    int lvl = g_config.current_level;

    for (int i = 0; i < g_total_aliens; i++) {
      Collectable &c = g_config.collectables[lvl][i];
      if (c.texture_id == -1)
        continue;

      float cx = -(c.y + 0.5f) * cw_3d;
      float cz = (c.x + 0.5f) * cw_3d;
      HMM_Vec3 alien_pos = HMM_V3(cx, 0.0f, cz);

      // Check AABB
      float aabb_hx = 0.8f;
      float aabb_hz = 0.8f;
      float cam_hx = 0.3f;
      float cam_hz = 0.3f;
      bool overlap_x =
          (g_camera.position.X + cam_hx >= alien_pos.X - aabb_hx) &&
          (g_camera.position.X - cam_hx <= alien_pos.X + aabb_hx);
      bool overlap_z =
          (g_camera.position.Z + cam_hz >= alien_pos.Z - aabb_hz) &&
          (g_camera.position.Z - cam_hz <= alien_pos.Z + aabb_hz);

      if (overlap_x && overlap_z) {
        c.texture_id = -1; // Marcar como recolectado
        g_collected_count++;

        // Si capturamos el último alien y no hay escaleras activas, crearlas
        if (g_collected_count == g_total_aliens && !g_stairs_active) {
          int best_z = -1, best_x = -1;
          float best_dist = 1e9f;
          int **matrix_st = g_config.levels[lvl].map->get_matrix();
          int size_st = g_config.levels[lvl].map->get_size();

          // Buscar el muro MÁS CERCANO al jugador que tenga al menos
          // un vecino navegable (para que el jugador pueda llegar hasta él).
          for (int z = 0; z < size_st; z++) {
            for (int x = 0; x < size_st; x++) {
              if (matrix_st[z][x] != 0)
                continue; // Solo muros (valor 0)

              // Verificar que al menos un vecino ortogonal sea navegable
              bool has_nav_neighbor = false;
              const int dz4[4] = {-1, 1, 0, 0};
              const int dx4[4] = {0, 0, -1, 1};
              for (int d = 0; d < 4; d++) {
                int nz = z + dz4[d], nx = x + dx4[d];
                if (nz >= 0 && nz < size_st && nx >= 0 && nx < size_st &&
                    matrix_st[nz][nx] != 0) {
                  has_nav_neighbor = true;
                  break;
                }
              }
              if (!has_nav_neighbor)
                continue;

              HMM_Vec3 wall_pos =
                  HMM_V3(-(z + 0.5f) * cw_3d, 0.0f, (x + 0.5f) * cw_3d);
              float dist = HMM_LenV3(HMM_SubV3(wall_pos, g_camera.position));
              if (dist < best_dist) {
                best_dist = dist;
                best_z = z;
                best_x = x;
              }
            }
          }
          if (best_z != -1 && best_x != -1) {
            matrix_st[best_z][best_x] = 99; // Marker de Escalera
            g_stairs_active = true;
            g_stairs_pos =
                HMM_V3(-(best_z + 0.5f) * cw_3d, 0.0f, (best_x + 0.5f) * cw_3d);
            build_level_geometry(lvl); // Reconstruir sin ese muro

            printf("=== PUERTA GENERADA en celda [%d][%d], posición 3D (%.1f, "
                   "%.1f) ===\n",
                   best_z, best_x, g_stairs_pos.X, g_stairs_pos.Z);

            return; // Exit frame to prevent double buffer updates from
                    // build_level_geometry and the main update below
          } else {
            printf("!!! ERROR: No se encontró muro adyacente a zona navegable "
                   "!!!\n");
          }
        }
        continue;
      }

      HMM_Mat4 scale = HMM_Scale(HMM_V3(0.25f, 0.25f, 0.25f));
      HMM_Mat4 move = HMM_Translate(alien_pos);
      HMM_Mat4 rot =
          HMM_Rotate_RH(HMM_AngleDeg(c.x * c.y * 30.0f), HMM_V3(0, 1, 0));
      HMM_Mat4 tf = HMM_MulM4(move, HMM_MulM4(rot, scale));

      if (c.texture_id == 100)
        cyclop_inst.push_back(tf);
      else if (c.texture_id == 101)
        scolitex_inst.push_back(tf);
      else
        oculi_inst.push_back(tf);
    }

    auto update_dynamic_buf = [](sg_buffer buf,
                                 const std::vector<HMM_Mat4> &inst) {
      if (!inst.empty()) {
        sg_update_buffer(buf, {inst.data(), inst.size() * sizeof(HMM_Mat4)});
      }
    };
    g_cyclop_count = cyclop_inst.size();
    g_scolitex_count = scolitex_inst.size();
    g_oculi_count = oculi_inst.size();
    update_dynamic_buf(g_alien_cyclop_inst_buf, cyclop_inst);
    update_dynamic_buf(g_alien_scolitex_inst_buf, scolitex_inst);
    update_dynamic_buf(g_alien_oculi_inst_buf, oculi_inst);

    // Colision con la puerta — disparar fade al tocar el area de la puerta
    if (g_stairs_active && g_fade_state == FADE_NONE) {
      // Usar la posicion real de la cara de la puerta (de su matriz de
      // transformación)
      HMM_Vec3 face_pos = HMM_V3(g_door_transform.Elements[3][0],
                                 g_door_transform.Elements[3][1],
                                 g_door_transform.Elements[3][2]);

      // AABB centrada en la puerta. Extensión de 1.5 unidades cubre el area de
      // la textura (ancho 4 -> half 2) y profundidad de +-1.0 para detectar la
      // colisión cerca.
      float hx = 1.5f;
      float hz = 1.5f;
      bool overlap_x = (g_camera.position.X + 0.3f >= face_pos.X - hx) &&
                       (g_camera.position.X - 0.3f <= face_pos.X + hx);
      bool overlap_z = (g_camera.position.Z + 0.3f >= face_pos.Z - hz) &&
                       (g_camera.position.Z - 0.3f <= face_pos.Z + hz);
      if (overlap_x && overlap_z) {
        g_fade_state = FADE_OUT;
        g_fade_alpha = 0.0f;
      }
    }

    // --- Fade state machine ---
    if (g_fade_state == FADE_OUT) {
      g_fade_alpha += FADE_SPEED * dt;
      if (g_fade_alpha >= 1.0f) {
        g_fade_alpha = 1.0f;

        int next_level = g_config.current_level + 1;
        if (next_level >= 3) {
          // ¡Victoria! No hay más niveles
          g_game_won = true;
          g_fade_state = FADE_NONE;
          g_fade_alpha = 0.0f;
          g_stairs_active = false;
          sapp_lock_mouse(
              false); // Liberar el cursor para la pantalla de victoria
        } else {
          // Pantalla completamente negra → cambiar de nivel
          g_config.current_level = next_level;
          lvl = g_config.current_level;
          g_stairs_active = false;
          g_total_aliens = g_config.collectable_count[lvl];
          g_collected_count = 0;

          build_level_geometry(lvl);

          int start_room_idx = rand() % g_config.levels[lvl].map->room_count;
          Room start_room = g_config.levels[lvl].map->rooms[start_room_idx];
          g_camera.position = HMM_V3(-start_room.center_y() * cw_3d, 1.0f,
                                     start_room.center_x() * cw_3d);
          g_camera.yaw = 180.0f;
          g_camera.update_vectors();

          g_fade_state = FADE_IN;
          return; // Prevents updating buffers twice in the same frame when
                  // transitioning levels
        }
      }
    } else if (g_fade_state == FADE_IN) {
      g_fade_alpha -= FADE_SPEED * dt;
      if (g_fade_alpha <= 0.0f) {
        g_fade_alpha = 0.0f;
        g_fade_state = FADE_NONE;
      }
    }
  } // end if (!g_game_won)

  // Actualizar buffer de puerta si es necesario
  if (g_door_needs_update) {
    g_door_needs_update = false;
    sg_update_buffer(g_door_inst_buf, {&g_door_transform, sizeof(HMM_Mat4)});
  }

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

  fs_params_t fs_params = {};
  fs_params.light_dir = HMM_V3(0.3f, -1.0f, 0.2f);

  // Rellenar luces de las salas
  int room_count = g_config.levels[g_config.current_level].map->room_count;
  int num_lights_to_send = std::min(room_count, MAX_ROOM_LIGHTS);
  fs_params.num_lights = (float)num_lights_to_send;

  for (int i = 0; i < num_lights_to_send; i++) {
    Room r = g_config.levels[g_config.current_level].map->rooms[i];

    // Centro geómetrico exacto (tomando en cuenta ancho/largo y el cw)
    // El 'Y' en grilla es el eje -X 3D, el 'X' en grilla es el eje Z 3D
    float mid_z = r.y + (r.h - 1) / 2.0f;
    float mid_x = r.x + (r.w - 1) / 2.0f;

    float world_x = -mid_z * cw;
    float world_z = mid_x * cw;
    float world_y = 2.0f; // Altura de luz

    // El radio debe iluminar la habitación pero detenerse en las paredes
    // Por eso tomamos el borde más ancho y le sumamos un margen leve
    float max_dim = (float)std::max(r.w, r.h);
    float radius = (max_dim / 2.0f + 0.8f) * cw;

    fs_params.room_lights[i] = HMM_V4(world_x, world_y, world_z, radius);
  }

  // Enviar posición del jugador y el radio de sub-iluminación
  fs_params.player_light = HMM_V4(g_camera.position.X, g_camera.position.Y,
                                  g_camera.position.Z, 15.0f);

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

  // Draw floors grouped by texture
  for (auto &it : g_floor_groups) {
    TextureGroup &group = it.second;
    if (group.num_instances <= 0)
      continue;

    sg_apply_pipeline(g_pip);
    sg_bindings f_binds = {};
    f_binds.vertex_buffers[0] = g_floor_mesh.vbuf;
    f_binds.vertex_buffers[1] = group.inst_buf;
    f_binds.index_buffer = g_floor_mesh.ibuf;

    f_binds.views[0] = group.diffuse_view;
    f_binds.views[1] = group.normal_view;
    f_binds.views[2] = group.orm_view;
    f_binds.views[3] = group.emissive_view;
    f_binds.samplers[0].id = g_sampler.id;
    sg_apply_bindings(&f_binds);

    sg_range vs_range = SG_RANGE(vs_params);
    sg_apply_uniforms(0, &vs_range);

    sg_range fs_range = SG_RANGE(fs_params);
    sg_apply_uniforms(1, &fs_range);

    // Draw this group of floor tiles
    sg_draw(0, g_floor_mesh.num_indices, group.num_instances);
  }

  // --- Draw Alien Collectables ---
  auto draw_aliens = [&](Mesh &mesh, sg_buffer ibuf, int cnt) {
    if (cnt <= 0 || mesh.num_indices <= 0 || ibuf.id == SG_INVALID_ID)
      return;
    sg_apply_pipeline(g_pip);
    sg_bindings b = {};
    b.vertex_buffers[0] = mesh.vbuf;
    b.vertex_buffers[1] = ibuf;
    b.index_buffer = mesh.ibuf;
    // Default to white/dummy if alien has no texture loaded (it has missing png
    // map)
    b.views[0] = (mesh.diffuse_view.id != SG_INVALID_ID) ? mesh.diffuse_view
                                                         : g_dummy_white_view;
    b.views[1] = (mesh.normal_view.id != SG_INVALID_ID) ? mesh.normal_view
                                                        : g_dummy_normal_view;
    b.views[2] =
        (mesh.orm_view.id != SG_INVALID_ID) ? mesh.orm_view : g_dummy_orm_view;
    b.views[3] = (mesh.emissive_view.id != SG_INVALID_ID) ? mesh.emissive_view
                                                          : g_dummy_black_view;
    b.samplers[0].id = g_sampler.id;
    sg_apply_bindings(&b);
    sg_range vs = SG_RANGE(vs_params);
    sg_apply_uniforms(0, &vs);
    sg_range fs = SG_RANGE(fs_params);
    sg_apply_uniforms(1, &fs);
    sg_draw(0, mesh.num_indices, cnt);
  };

  draw_aliens(g_alien_cyclop_mesh, g_alien_cyclop_inst_buf, g_cyclop_count);
  draw_aliens(g_alien_scolitex_mesh, g_alien_scolitex_inst_buf,
              g_scolitex_count);
  draw_aliens(g_alien_oculi_mesh, g_alien_oculi_inst_buf, g_oculi_count);

  // --- Dibujar la puerta si está activa ---
  if (g_stairs_active && g_door_mesh.num_indices > 0) {
    sg_apply_pipeline(g_pip);
    sg_bindings b = {};
    b.vertex_buffers[0] = g_door_mesh.vbuf;
    b.vertex_buffers[1] = g_door_inst_buf;
    b.index_buffer = g_door_mesh.ibuf;
    b.views[0] = (g_door_mesh.diffuse_view.id != SG_INVALID_ID)
                     ? g_door_mesh.diffuse_view
                     : g_dummy_white_view;
    b.views[1] = (g_door_mesh.normal_view.id != SG_INVALID_ID)
                     ? g_door_mesh.normal_view
                     : g_dummy_normal_view;
    b.views[2] = (g_door_mesh.orm_view.id != SG_INVALID_ID)
                     ? g_door_mesh.orm_view
                     : g_dummy_orm_view;
    b.views[3] = (g_door_mesh.emissive_view.id != SG_INVALID_ID)
                     ? g_door_mesh.emissive_view
                     : g_dummy_black_view;
    b.samplers[0].id = g_sampler.id;
    sg_apply_bindings(&b);
    sg_range vs = SG_RANGE(vs_params);
    sg_apply_uniforms(0, &vs);
    sg_range fs = SG_RANGE(fs_params);
    sg_apply_uniforms(1, &fs);
    sg_draw(0, g_door_mesh.num_indices, 1);
  }

  // --- Minimap 2D en la esquina inferior derecha (no dibujar si ganó) ---
  if (!g_game_won) {
    // Definir el rectángulo del minimapa en NDC ([-1,1] x [-1,1])
    float margin = 0.05f;
    float map_w = 0.35f;
    float map_h = 0.35f;
    float right = 1.0f - margin;
    float bottom = -1.0f + margin;
    float left = right - map_w;
    float top = bottom + map_h;

    float cell_w = map_w / (float)size;
    float cell_h = map_h / (float)size;

    MinimapVertex verts[MAX_MINIMAP_VERTS];
    int vcount = 0;

    // Dibujar celdas navegables como cuadriculas grises oscuras
    for (int z = 0; z < size; z++) {
      for (int x = 0; x < size; x++) {
        if (matrix[z][x] == 0)
          continue;

        if (vcount + 6 > MAX_MINIMAP_VERTS)
          break;

        float x0 = left + x * cell_w;
        float x1 = left + (x + 1) * cell_w;
        float y0 = bottom + z * cell_h;
        float y1 = bottom + (z + 1) * cell_h;

        // Color gris oscuro semi-transparente
        float r = 0.15f, g = 0.15f, b = 0.15f, a = 0.8f;

        // Dos triángulos por celda
        verts[vcount++] = {x0, y0, r, g, b, a};
        verts[vcount++] = {x1, y0, r, g, b, a};
        verts[vcount++] = {x1, y1, r, g, b, a};

        verts[vcount++] = {x0, y0, r, g, b, a};
        verts[vcount++] = {x1, y1, r, g, b, a};
        verts[vcount++] = {x0, y1, r, g, b, a};
      }
    }

    auto append_char = [&](char ch, float base_x, float base_y, float cw_txt,
                           float ch_txt, float r, float g, float b, float a) {
      // Fuente 3x5 muy simple en forma de matriz de bits.
      // Cada fila es un entero con 3 bits significativos.
      uint8_t rows[5] = {0, 0, 0, 0, 0};
      switch (ch) {
      case 'L':
      case 'l':
        rows[0] = 4;
        rows[1] = 4;
        rows[2] = 4;
        rows[3] = 4;
        rows[4] = 7;
        break;
      case 'o':
      case 'O':
        rows[0] = 2;
        rows[1] = 5;
        rows[2] = 5;
        rows[3] = 5;
        rows[4] = 2;
        break;
      case 'c':
      case 'C':
        rows[0] = 3;
        rows[1] = 4;
        rows[2] = 4;
        rows[3] = 4;
        rows[4] = 3;
        break;
      case 'a':
      case 'A':
        rows[0] = 2;
        rows[1] = 5;
        rows[2] = 7;
        rows[3] = 5;
        rows[4] = 5;
        break;
      case 'i':
      case 'I':
        rows[0] = 7;
        rows[1] = 2;
        rows[2] = 2;
        rows[3] = 2;
        rows[4] = 7;
        break;
      case 'z':
      case 'Z':
        rows[0] = 7;
        rows[1] = 1;
        rows[2] = 2;
        rows[3] = 4;
        rows[4] = 7;
        break;
      case 'd':
      case 'D':
        rows[0] = 3;
        rows[1] = 5;
        rows[2] = 5;
        rows[3] = 5;
        rows[4] = 3;
        break;
      case ':':
        rows[0] = 0;
        rows[1] = 2;
        rows[2] = 0;
        rows[3] = 2;
        rows[4] = 0;
        break;
      case '[':
        rows[0] = 3;
        rows[1] = 2;
        rows[2] = 2;
        rows[3] = 2;
        rows[4] = 3;
        break;
      case ']':
        rows[0] = 6;
        rows[1] = 2;
        rows[2] = 2;
        rows[3] = 2;
        rows[4] = 6;
        break;
      case 's':
      case 'S':
        rows[0] = 3;
        rows[1] = 4;
        rows[2] = 2;
        rows[3] = 1;
        rows[4] = 6;
        break;
      case '-':
        rows[0] = 0;
        rows[1] = 0;
        rows[2] = 7;
        rows[3] = 0;
        rows[4] = 0;
        break;
      case '0':
        rows[0] = 7;
        rows[1] = 5;
        rows[2] = 5;
        rows[3] = 5;
        rows[4] = 7;
        break;
      case '1':
        rows[0] = 2;
        rows[1] = 6;
        rows[2] = 2;
        rows[3] = 2;
        rows[4] = 7;
        break;
      case '2':
        rows[0] = 7;
        rows[1] = 1;
        rows[2] = 7;
        rows[3] = 4;
        rows[4] = 7;
        break;
      case '3':
        rows[0] = 7;
        rows[1] = 1;
        rows[2] = 7;
        rows[3] = 1;
        rows[4] = 7;
        break;
      case '4':
        rows[0] = 5;
        rows[1] = 5;
        rows[2] = 7;
        rows[3] = 1;
        rows[4] = 1;
        break;
      case '5':
        rows[0] = 7;
        rows[1] = 4;
        rows[2] = 7;
        rows[3] = 1;
        rows[4] = 7;
        break;
      case '6':
        rows[0] = 7;
        rows[1] = 4;
        rows[2] = 7;
        rows[3] = 5;
        rows[4] = 7;
        break;
      case '7':
        rows[0] = 7;
        rows[1] = 1;
        rows[2] = 2;
        rows[3] = 2;
        rows[4] = 2;
        break;
      case '8':
        rows[0] = 7;
        rows[1] = 5;
        rows[2] = 7;
        rows[3] = 5;
        rows[4] = 7;
        break;
      case '9':
        rows[0] = 7;
        rows[1] = 5;
        rows[2] = 7;
        rows[3] = 1;
        rows[4] = 7;
        break;
      case 'p':
      case 'P':
        rows[0] = 7;
        rows[1] = 5;
        rows[2] = 7;
        rows[3] = 4;
        rows[4] = 4;
        break;
      case 't':
      case 'T':
        rows[0] = 7;
        rows[1] = 2;
        rows[2] = 2;
        rows[3] = 2;
        rows[4] = 2;
        break;
      case 'u':
      case 'U':
        rows[0] = 5;
        rows[1] = 5;
        rows[2] = 5;
        rows[3] = 5;
        rows[4] = 7;
        break;
      case 'R':
      case 'r':
        rows[0] = 7;
        rows[1] = 5;
        rows[2] = 7;
        rows[3] = 6;
        rows[4] = 5;
        break;
      case 'N':
      case 'n':
        rows[0] = 5;
        rows[1] = 7;
        rows[2] = 5;
        rows[3] = 5;
        rows[4] = 5;
        break;
      case 'F':
      case 'f':
        rows[0] = 7;
        rows[1] = 4;
        rows[2] = 6;
        rows[3] = 4;
        rows[4] = 4;
        break;
      case 'E':
      case 'e':
        rows[0] = 7;
        rows[1] = 4;
        rows[2] = 7;
        rows[3] = 4;
        rows[4] = 7;
        break;
      case ' ':
      default:
        break; // space or unknown
      }

      for (int r_idx = 0; r_idx < 5; r_idx++) {
        for (int c_idx = 0; c_idx < 3; c_idx++) {
          if (rows[r_idx] & (1 << (2 - c_idx))) {
            if (vcount + 6 > MAX_MINIMAP_VERTS)
              return;

            float px0 = base_x + c_idx * cw_txt;
            float px1 = px0 + cw_txt;
            float py1 = base_y - r_idx * ch_txt; // Dibuja hacia abajo
            float py0 = py1 - ch_txt;

            verts[vcount++] = {px0, py0, r, g, b, a};
            verts[vcount++] = {px1, py0, r, g, b, a};
            verts[vcount++] = {px1, py1, r, g, b, a};

            verts[vcount++] = {px0, py0, r, g, b, a};
            verts[vcount++] = {px1, py1, r, g, b, a};
            verts[vcount++] = {px0, py1, r, g, b, a};
          }
        }
      }
    };

    float window_w = (float)sapp_width();
    float window_h = (float)sapp_height();

    // Reducimos el tamaño global de las fuentes un 20% (multiplicando por 0.8)
    float text_scale_x = 0.05f * 0.80f;
    float text_scale_y = (window_w / (float)window_h) * 0.05f * 0.80f;
    float cw_txt = text_scale_x / 4.0f;
    float ch_txt = text_scale_y / 6.0f;

    // Dibujar UI contador de Aliens (Top-Left)
    std::string alien_label;
    if (g_collected_count >= g_total_aliens) {
      alien_label = "TODOS LOS ALIENS FUERON CAPTURADOS";
    } else {
      alien_label = "ALIENS CAPTURADOS: " + std::to_string(g_collected_count) +
                    " DE " + std::to_string(g_total_aliens);
    }
    float ui_x = -0.95f;
    float ui_y = 0.90f;
    for (char c : alien_label) {
      append_char(c, ui_x, ui_y, cw_txt, ch_txt, 0.2f, 1.0f, 0.5f,
                  1.0f); // Verde acentuado
      ui_x += cw_txt * 4.0f;
    }

    // Marcar posición del jugador en verde
    if (current_z >= 0 && current_z < size && current_x >= 0 &&
        current_x < size) {
      if (vcount + 6 <= MAX_MINIMAP_VERTS) {
        float px0 = left + current_x * cell_w + cell_w * 0.15f;
        float px1 = left + (current_x + 1) * cell_w - cell_w * 0.15f;
        float py0 = bottom + current_z * cell_h + cell_h * 0.15f;
        float py1 = bottom + (current_z + 1) * cell_h - cell_h * 0.15f;

        float r = 0.0f, g = 0.9f, b = 0.2f, a = 1.0f;

        verts[vcount++] = {px0, py0, r, g, b, a};
        verts[vcount++] = {px1, py0, r, g, b, a};
        verts[vcount++] = {px1, py1, r, g, b, a};

        verts[vcount++] = {px0, py0, r, g, b, a};
        verts[vcount++] = {px1, py1, r, g, b, a};
        verts[vcount++] = {px0, py1, r, g, b, a};
      }
    }

    // Dibujar puntos rojos para los aliens no recolectados si el flag está
    // activo
    if (g_show_minimap_aliens) {
      int lvl_map = g_config.current_level;
      for (int i = 0; i < g_total_aliens; i++) {
        Collectable &c = g_config.collectables[lvl_map][i];
        if (c.texture_id == -1) // Ignorar los ya recolectados
          continue;

        if (vcount + 6 <= MAX_MINIMAP_VERTS) {
          // El alien está en c.y (fila/z) y c.x (columna/x)
          float px0 = left + c.x * cell_w + cell_w * 0.3f;
          float px1 = left + (c.x + 1) * cell_w - cell_w * 0.3f;
          float py0 = bottom + c.y * cell_h + cell_h * 0.3f;
          float py1 = bottom + (c.y + 1) * cell_h - cell_h * 0.3f;

          float r = 1.0f, g = 0.0f, b = 0.0f, a = 1.0f; // Rojo puro

          verts[vcount++] = {px0, py0, r, g, b, a};
          verts[vcount++] = {px1, py0, r, g, b, a};
          verts[vcount++] = {px1, py1, r, g, b, a};

          verts[vcount++] = {px0, py0, r, g, b, a};
          verts[vcount++] = {px1, py1, r, g, b, a};
          verts[vcount++] = {px0, py1, r, g, b, a};
        }
      }
    }

    if (vcount > 0) {
      sg_apply_pipeline(g_minimap_pip);
      sg_bindings mm_binds = {};
      mm_binds.vertex_buffers[0] = g_minimap_vbuf;
      sg_apply_bindings(&mm_binds);

      sg_range mm_range = {verts, (size_t)(vcount * sizeof(MinimapVertex))};
      sg_update_buffer(g_minimap_vbuf, &mm_range);

      sg_draw(0, vcount, 1);
    }
  }
  // --- Fade Overlay (pantalla negra con alpha para transición) ---
  if (g_fade_state != FADE_NONE || g_game_won) {
    MinimapVertex fade_verts[6];
    float a = g_game_won ? 1.0f : g_fade_alpha;
    // Triángulo 1
    fade_verts[0] = {-1.0f, -1.0f, 0, 0, 0, a};
    fade_verts[1] = {1.0f, -1.0f, 0, 0, 0, a};
    fade_verts[2] = {1.0f, 1.0f, 0, 0, 0, a};
    // Triángulo 2
    fade_verts[3] = {-1.0f, -1.0f, 0, 0, 0, a};
    fade_verts[4] = {1.0f, 1.0f, 0, 0, 0, a};
    fade_verts[5] = {-1.0f, 1.0f, 0, 0, 0, a};

    sg_apply_pipeline(g_minimap_pip);
    sg_bindings fade_binds = {};
    fade_binds.vertex_buffers[0] = g_fade_vbuf;
    sg_apply_bindings(&fade_binds);

    sg_range fade_range = {fade_verts, sizeof(fade_verts)};
    sg_update_buffer(g_fade_vbuf, &fade_range);
    sg_draw(0, 6, 1);
  }

  // --- Pantalla de Victoria ---
  if (g_game_won) {
    float window_w = (float)sapp_width();
    float window_h = (float)sapp_height();
    float text_scale_x = 0.04f;
    float text_scale_y = (window_w / window_h) * 0.04f;
    float cw_txt = text_scale_x / 4.0f;
    float ch_txt = text_scale_y / 6.0f;
    int vcount_win = 0;
    MinimapVertex win_verts[MAX_MINIMAP_VERTS];

    auto append_win_char = [&](char ch, float base_x, float base_y, float cw_c,
                               float ch_c, float r, float g, float b,
                               float aa) {
      uint8_t rows[5] = {0, 0, 0, 0, 0};
      switch (ch) {
      case 'A':
        rows[0] = 0b010;
        rows[1] = 0b101;
        rows[2] = 0b111;
        rows[3] = 0b101;
        rows[4] = 0b101;
        break;
      case 'B':
        rows[0] = 0b110;
        rows[1] = 0b101;
        rows[2] = 0b110;
        rows[3] = 0b101;
        rows[4] = 0b110;
        break;
      case 'C':
        rows[0] = 0b111;
        rows[1] = 0b100;
        rows[2] = 0b100;
        rows[3] = 0b100;
        rows[4] = 0b111;
        break;
      case 'D':
        rows[0] = 0b110;
        rows[1] = 0b101;
        rows[2] = 0b101;
        rows[3] = 0b101;
        rows[4] = 0b110;
        break;
      case 'E':
        rows[0] = 0b111;
        rows[1] = 0b100;
        rows[2] = 0b110;
        rows[3] = 0b100;
        rows[4] = 0b111;
        break;
      case 'G':
        rows[0] = 0b111;
        rows[1] = 0b100;
        rows[2] = 0b101;
        rows[3] = 0b101;
        rows[4] = 0b111;
        break;
      case 'H':
        rows[0] = 0b101;
        rows[1] = 0b101;
        rows[2] = 0b111;
        rows[3] = 0b101;
        rows[4] = 0b101;
        break;
      case 'I':
        rows[0] = 0b111;
        rows[1] = 0b010;
        rows[2] = 0b010;
        rows[3] = 0b010;
        rows[4] = 0b111;
        break;
      case 'J':
        rows[0] = 0b001;
        rows[1] = 0b001;
        rows[2] = 0b001;
        rows[3] = 0b101;
        rows[4] = 0b010;
        break;
      case 'L':
        rows[0] = 0b100;
        rows[1] = 0b100;
        rows[2] = 0b100;
        rows[3] = 0b100;
        rows[4] = 0b111;
        break;
      case 'N':
        rows[0] = 0b101;
        rows[1] = 0b111;
        rows[2] = 0b111;
        rows[3] = 0b101;
        rows[4] = 0b101;
        break;
      case 'O':
        rows[0] = 0b010;
        rows[1] = 0b101;
        rows[2] = 0b101;
        rows[3] = 0b101;
        rows[4] = 0b010;
        break;
      case 'P':
        rows[0] = 0b110;
        rows[1] = 0b101;
        rows[2] = 0b110;
        rows[3] = 0b100;
        rows[4] = 0b100;
        break;
      case 'R':
        rows[0] = 0b110;
        rows[1] = 0b101;
        rows[2] = 0b110;
        rows[3] = 0b101;
        rows[4] = 0b101;
        break;
      case 'S':
        rows[0] = 0b011;
        rows[1] = 0b100;
        rows[2] = 0b010;
        rows[3] = 0b001;
        rows[4] = 0b110;
        break;
      case 'U':
        rows[0] = 0b101;
        rows[1] = 0b101;
        rows[2] = 0b101;
        rows[3] = 0b101;
        rows[4] = 0b111;
        break;
      case ',':
        rows[3] = 0b010;
        rows[4] = 0b100;
        break;
      case ' ':
        break;
      default:
        rows[0] = 0b111;
        rows[1] = 0b111;
        rows[2] = 0b111;
        rows[3] = 0b111;
        rows[4] = 0b111;
        break;
      }
      for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
          if (rows[row] & (1 << (2 - col))) {
            float x0 = base_x + col * cw_c;
            float y0 = base_y - row * ch_c;
            float x1 = x0 + cw_c;
            float y1 = y0 - ch_c;
            if (vcount_win + 6 <= MAX_MINIMAP_VERTS) {
              win_verts[vcount_win++] = {x0, y0, r, g, b, aa};
              win_verts[vcount_win++] = {x1, y0, r, g, b, aa};
              win_verts[vcount_win++] = {x1, y1, r, g, b, aa};
              win_verts[vcount_win++] = {x0, y0, r, g, b, aa};
              win_verts[vcount_win++] = {x1, y1, r, g, b, aa};
              win_verts[vcount_win++] = {x0, y1, r, g, b, aa};
            }
          }
        }
      }
    };

    // Linea 1:  HAS GANADO
    std::string line1 = "HAS GANADO";
    float line1_w = line1.size() * 4 * cw_txt;
    float x1_start = -line1_w / 2.0f;
    float y1_start = 0.3f;
    for (size_t i = 0; i < line1.size(); i++) {
      append_win_char(line1[i], x1_start + i * 4 * cw_txt, y1_start, cw_txt,
                      ch_txt, 0.2f, 1.0f, 0.4f, 1.0f);
    }

    // Linea 2:  PRESIONA ESPACIO
    std::string line2 = "PRESIONA ESPACIO";
    float line2_w = line2.size() * 4 * cw_txt;
    float x2_start = -line2_w / 2.0f;
    float y2_start = 0.0f;
    for (size_t i = 0; i < line2.size(); i++) {
      append_win_char(line2[i], x2_start + i * 4 * cw_txt, y2_start, cw_txt,
                      ch_txt, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    // Linea 3:  PARA REINICIAR
    std::string line3 = "PARA REINICIAR";
    float line3_w = line3.size() * 4 * cw_txt;
    float x3_start = -line3_w / 2.0f;
    float y3_start = -0.2f;
    for (size_t i = 0; i < line3.size(); i++) {
      append_win_char(line3[i], x3_start + i * 4 * cw_txt, y3_start, cw_txt,
                      ch_txt, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    // Linea 4:  O ESC PARA SALIR
    std::string line4 = "O ESC PARA SALIR";
    float line4_w = line4.size() * 4 * cw_txt;
    float x4_start = -line4_w / 2.0f;
    float y4_start = -0.4f;
    for (size_t i = 0; i < line4.size(); i++) {
      append_win_char(line4[i], x4_start + i * 4 * cw_txt, y4_start, cw_txt,
                      ch_txt, 0.7f, 0.7f, 0.7f, 1.0f);
    }

    if (vcount_win > 0) {
      sg_apply_pipeline(g_minimap_pip);
      sg_bindings win_binds = {};
      win_binds.vertex_buffers[0] = g_minimap_vbuf;
      sg_apply_bindings(&win_binds);
      sg_range win_range = {win_verts,
                            (size_t)(vcount_win * sizeof(MinimapVertex))};
      sg_update_buffer(g_minimap_vbuf, &win_range);
      sg_draw(0, vcount_win, 1);
    }
  }

  sg_end_pass();
  sg_commit();
}

// Callback: se ejecuta al cerrar la aplicación
static void cleanup_cb(void) {
  g_wall_mesh.destroy();
  g_floor_mesh.destroy();
  g_alien_cyclop_mesh.destroy();
  g_alien_scolitex_mesh.destroy();
  g_alien_oculi_mesh.destroy();
  g_door_mesh.destroy(); // Destroy door mesh

  // Limpiar grupos de suelo
  for (auto &it : g_floor_groups) {
    it.second.destroy();
  }
  g_floor_groups.clear();

  if (g_alien_cyclop_inst_buf.id != SG_INVALID_ID)
    sg_destroy_buffer(g_alien_cyclop_inst_buf);
  if (g_alien_scolitex_inst_buf.id != SG_INVALID_ID)
    sg_destroy_buffer(g_alien_scolitex_inst_buf);
  if (g_alien_oculi_inst_buf.id != SG_INVALID_ID)
    sg_destroy_buffer(g_alien_oculi_inst_buf);

  sg_destroy_buffer(g_inst_buf);
  sg_destroy_pipeline(g_pip);
  sg_destroy_buffer(g_minimap_vbuf);
  sg_destroy_pipeline(g_minimap_pip);
  sg_destroy_shader(g_minimap_shader);
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
    // Reiniciar juego desde la pantalla de victoria
    if (ev->key_code == SAPP_KEYCODE_SPACE && g_game_won) {
      g_restart_pending = true; // Diferir al frame_cb
    }
    // Toggle para mostrar aliens en el minimapa
    if (ev->key_code == SAPP_KEYCODE_M) {
      g_show_minimap_aliens = !g_show_minimap_aliens;
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
