#ifndef OBJ_LOADER_HPP
#define OBJ_LOADER_HPP

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "HandmadeMath.h"
#include "sokol/sokol_gfx.h"
#include <iostream>
#include <string>
#include <vector>

struct Vertex {
  HMM_Vec3 pos;
  HMM_Vec3 normal;
  HMM_Vec2 uv;
};

struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  sg_buffer vbuf;
  sg_buffer ibuf;
  sg_image diffuse_img;
  sg_view diffuse_view;
  sg_image normal_img;
  sg_view normal_view;
  sg_image orm_img;
  sg_view orm_view;
  sg_image emissive_img;
  sg_view emissive_view;

  int num_indices;

  void destroy() {
    sg_destroy_buffer(vbuf);
    sg_destroy_buffer(ibuf);
    if (diffuse_img.id != SG_INVALID_ID)
      sg_destroy_image(diffuse_img);
    if (normal_img.id != SG_INVALID_ID)
      sg_destroy_image(normal_img);
    if (orm_img.id != SG_INVALID_ID)
      sg_destroy_image(orm_img);
    if (emissive_img.id != SG_INVALID_ID)
      sg_destroy_image(emissive_img);
  }
};

inline void load_textures(const std::string &tex_path, Mesh &out_mesh) {
  if (tex_path.empty())
    return;
  // Helper function to load complementary textures
  auto load_sub_tex = [&](const std::string &suffix, sg_image &out_img,
                          sg_view &out_view) {
    std::string path = tex_path;

    // Replace "_BaseColor" with the suffix if it exists
    size_t base_color_idx = path.find("_BaseColor");
    std::string sub_path;

    if (base_color_idx != std::string::npos) {
      sub_path = path;
      sub_path.replace(base_color_idx, 10,
                       suffix); // 10 is length of "_BaseColor"
    } else {
      // Fallback: just append before the extension
      size_t dot_pos = path.rfind('.');
      if (dot_pos != std::string::npos) {
        std::string base = path.substr(0, dot_pos);
        std::string ext = path.substr(dot_pos);
        sub_path = base + suffix + ext;
      } else {
        sub_path = path + suffix;
      }
    }

    int w, h, c;
    stbi_uc *pix = stbi_load(sub_path.c_str(), &w, &h, &c, 4);
    if (pix) {
      sg_image_desc d = {};
      d.width = w;
      d.height = h;
      d.pixel_format = SG_PIXELFORMAT_RGBA8;
      d.data.mip_levels[0].ptr = pix;
      d.data.mip_levels[0].size = (size_t)(w * h * 4);
      out_img = sg_make_image(&d);

      sg_view_desc vd = {};
      vd.texture.image = out_img;
      out_view = sg_make_view(&vd);
      stbi_image_free(pix);
      return true;
    }
    return false;
  };

  stbi_set_flip_vertically_on_load(false);
  int width, height, channels;
  stbi_uc *pixels = stbi_load(tex_path.c_str(), &width, &height, &channels, 4);
  if (pixels) {
    sg_image_desc img_desc = {};
    img_desc.width = width;
    img_desc.height = height;
    img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    img_desc.data.mip_levels[0].ptr = pixels;
    img_desc.data.mip_levels[0].size = (size_t)(width * height * 4);
    out_mesh.diffuse_img = sg_make_image(&img_desc);

    sg_view_desc view_desc = {};
    view_desc.texture.image = out_mesh.diffuse_img;
    out_mesh.diffuse_view = sg_make_view(&view_desc);

    stbi_image_free(pixels);

    // Attempt to load Normal, ORM, and Emissive
    load_sub_tex("_Normal", out_mesh.normal_img, out_mesh.normal_view);
    load_sub_tex("_ORM", out_mesh.orm_img, out_mesh.orm_view);
    load_sub_tex("_Emissive", out_mesh.emissive_img, out_mesh.emissive_view);
  } else {
    std::cerr << "Failed to load texture: " << tex_path << std::endl;
  }
}

inline bool load_obj(const std::string &obj_path, const std::string &tex_path,
                     Mesh &out_mesh) {
  tinyobj::ObjReaderConfig reader_config;
  reader_config.triangulate = true;

  std::string base_dir = "";
  size_t last_slash_idx = obj_path.rfind('/');
  if (std::string::npos != last_slash_idx) {
    base_dir = obj_path.substr(0, last_slash_idx + 1);
  }
  reader_config.mtl_search_path = base_dir;

  tinyobj::ObjReader reader;

  // Pasamos el base_dir para que lea el archivo .mtl
  if (!reader.ParseFromFile(obj_path, reader_config)) {
    if (!reader.Error().empty())
      std::cerr << "ObjLoader Error: " << reader.Error();
    return false;
  }
  if (!reader.Warning().empty())
    std::cout << "ObjLoader Warning: " << reader.Warning();

  auto &attrib = reader.GetAttrib();
  auto &shapes = reader.GetShapes();

  // Simple load: Just dump all shape faces into one vertex list
  // Assumes obj has normals and UVs
  for (size_t s = 0; s < shapes.size(); s++) {
    size_t index_offset = 0;
    for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
      int fv = shapes[s].mesh.num_face_vertices[f];
      for (size_t v = 0; v < fv; v++) {
        tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];

        Vertex vertex;
        vertex.pos.X = attrib.vertices[3 * size_t(idx.vertex_index) + 0];
        vertex.pos.Y = attrib.vertices[3 * size_t(idx.vertex_index) + 1];
        vertex.pos.Z = attrib.vertices[3 * size_t(idx.vertex_index) + 2];

        if (idx.normal_index >= 0) {
          vertex.normal.X = attrib.normals[3 * size_t(idx.normal_index) + 0];
          vertex.normal.Y = attrib.normals[3 * size_t(idx.normal_index) + 1];
          vertex.normal.Z = attrib.normals[3 * size_t(idx.normal_index) + 2];
        } else {
          vertex.normal = HMM_V3(0, 1, 0);
        }

        if (idx.texcoord_index >= 0) {
          vertex.uv.X = attrib.texcoords[2 * size_t(idx.texcoord_index) + 0];
          vertex.uv.Y =
              1.0f -
              attrib.texcoords[2 * size_t(idx.texcoord_index) + 1]; // Flip V
        } else {
          vertex.uv = HMM_V2(0, 0);
        }

        out_mesh.vertices.push_back(vertex);
        out_mesh.indices.push_back((uint32_t)out_mesh.indices.size());
      }
      index_offset += fv;
    }
  }

  out_mesh.num_indices = (int)out_mesh.indices.size();

  // Create Sokol buffers
  sg_buffer_desc vbuf_desc = {};
  vbuf_desc.usage.vertex_buffer = true;
  vbuf_desc.data = {out_mesh.vertices.data(),
                    out_mesh.vertices.size() * sizeof(Vertex)};
  out_mesh.vbuf = sg_make_buffer(&vbuf_desc);

  sg_buffer_desc ibuf_desc = {};
  ibuf_desc.usage.index_buffer = true;
  ibuf_desc.data = {out_mesh.indices.data(),
                    out_mesh.indices.size() * sizeof(uint32_t)};
  out_mesh.ibuf = sg_make_buffer(&ibuf_desc);

  load_textures(tex_path, out_mesh);

  return true;
}

#endif
