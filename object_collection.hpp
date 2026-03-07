#ifndef OBJECT_COLLECTION_HPP
#define OBJECT_COLLECTION_HPP

#include "map_array_generator.hpp"
#include <cstdlib>

struct Collectable {
  int texture_id; // Valor entre 6 y 10
  int x, y;       // Posición en el mapa

  Collectable() : texture_id(0), x(0), y(0) {}

  Collectable(int tex_id, int pos_x, int pos_y)
      : texture_id(tex_id), x(pos_x), y(pos_y) {}
};

// Genera un número aleatorio de colectables (entre 4 y 7)
// y los coloca en posiciones aleatorias dentro de salas aleatorias
inline int place_collectables(Collectable *collectables, Room *rooms,
                              int room_count, int **matrix) {
  if (room_count == 0)
    return 0;

  // Generar entre 4 y 7 items
  int num_items = 4 + (rand() % 4);
  int placed = 0;

  for (int i = 0; i < num_items; i++) {
    // Seleccionar una sala aleatoria
    int room_idx = rand() % room_count;
    Room &room = rooms[room_idx];

    // Posición aleatoria dentro de la sala
    int px = room.x + (rand() % room.w);
    int py = room.y + (rand() % room.h);

    // Textura aleatoria entre 6 y 10
    int tex_id = 6 + (rand() % 5);

    collectables[placed] = Collectable(tex_id, px, py);

    // Marcar la posición en la matriz
    matrix[py][px] = tex_id;

    placed++;
  }

  return placed;
}

#endif // OBJECT_COLLECTION_HPP
