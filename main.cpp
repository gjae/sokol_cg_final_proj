#include "level.hpp"
#include "object_collection.hpp"

int main() {
  int base_size = 10;
  Level levels[3];
  create_levels(levels, base_size);

  for (int i = 0; i < 3; i++) {
    Collectable collectables[7]; // Máximo 7 items

    int placed = place_collectables(collectables, levels[i].map->rooms,
                                    levels[i].map->room_count,
                                    levels[i].map->get_matrix());

    levels[i].print();

    std::cout << "Colectables colocados: " << placed << std::endl;
    for (int j = 0; j < placed; j++) {
      std::cout << "  - Item " << j + 1
                << ": textura=" << collectables[j].texture_id << " pos=("
                << collectables[j].x << "," << collectables[j].y << ")"
                << std::endl;
    }
    std::cout << std::endl;
  }

  return 0;
}
