#include "game.hpp"

int main() {
  GameConfig config;
  int base_size = 20;

  // Crear los 3 niveles
  create_levels(config.levels, base_size);

  // Colocar colectables en cada nivel
  for (int i = 0; i < 3; i++) {
    config.collectable_count[i] = place_collectables(
        config.collectables[i], config.levels[i].map->rooms,
        config.levels[i].map->room_count, config.levels[i].map->get_matrix());

    config.levels[i].print();
    std::cout << "Colectables: " << config.collectable_count[i] << std::endl;
  }

  // Iniciar el juego con la configuración generada
  run_game(&config);

  return 0;
}
