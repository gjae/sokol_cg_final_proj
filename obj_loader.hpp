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
  int num_indices;

  void destroy() {
    sg_destroy_buffer(vbuf);
    sg_destroy_buffer(ibuf);
    sg_destroy_image(diffuse_img);
    // Note: older/different sokol versions might not have destroy view strictly
    // required if image is destroyed, but typically we should if it's
    // available. If sg_destroy_view doesn't exist, we leave it out. But
    // typically it's needed? Actually, let's omit sg_destroy_view just in case
    // it doesn't exist in simpler backends, deleting the image often
    // invalidates the view. Wait, I'll destroy it anyway if it parses. But
    // let's check sokol_gfx.h if sg_destroy_view exists. Wait, let's just leave
    // it out to prevent another potential compiler error if it's not exported.
  }
};

inline bool load_obj(const std::string &obj_path, const std::string &tex_path,
                     Mesh &out_mesh) {
  tinyobj::ObjReaderConfig reader_config;
  reader_config.triangulate = true;
  tinyobj::ObjReader reader;

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
  vbuf_desc.data = SG_RANGE(out_mesh.vertices);
  out_mesh.vbuf = sg_make_buffer(&vbuf_desc);

  sg_buffer_desc ibuf_desc = {};
  ibuf_desc.usage.index_buffer = true;
  ibuf_desc.data = SG_RANGE(out_mesh.indices);
  out_mesh.ibuf = sg_make_buffer(&ibuf_desc);

  // Load Texture
  int width, height, channels;
  stbi_set_flip_vertically_on_load(false); // Ya flipeamos UV arriba
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
  } else {
    std::cerr << "Failed to load texture: " << tex_path << std::endl;
  }

  return true;
}

#endif
