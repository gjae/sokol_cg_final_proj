#ifndef LEVEL_HPP
#define LEVEL_HPP

#include "map_array_generator.hpp"

struct Level {
  MapGenerator *map;
  int level;
  bool was_passed;

  Level() : map(nullptr), level(0), was_passed(false) {}

  // Inicializa el nivel con su número y tamaño de mapa
  void init(int lvl, int map_size) {
    level = lvl;
    was_passed = false;
    if (map != nullptr) {
      delete map;
    }
    map = new MapGenerator(map_size);
  }

  ~Level() {
    if (map != nullptr) {
      delete map;
      map = nullptr;
    }
  }

  void print() {
    if (map == nullptr)
      return;
    std::cout << "=== Nivel " << level << " (mapa " << map->get_size() << "x"
              << map->get_size() << ") ===" << std::endl;
    map->print_matrix();
    std::cout << std::endl;
  }
};

// Genera los 3 niveles con tamaño duplicándose
// Nivel 1: base_size, Nivel 2: base_size*2, Nivel 3: base_size*4
inline void create_levels(Level levels[3], int base_size) {
  for (int i = 0; i < 3; i++) {
    int size = base_size * (1 << i);
    levels[i].init(i + 1, size);
  }
}

#endif // LEVEL_HPP
