#ifndef GAME_HPP
#define GAME_HPP

#include "level.hpp"
#include "object_collection.hpp"

// Configuración del juego que se pasa desde main
struct GameConfig {
  Level levels[3];
  Collectable collectables[3][7]; // Colectables por nivel (max 7)
  int collectable_count[3];       // Cantidad real de colectables por nivel
  int current_level;

  GameConfig() : current_level(0) {
    for (int i = 0; i < 3; i++) {
      collectable_count[i] = 0;
    }
  }
};

// Punto de entrada del juego, invocado desde main
void run_game(GameConfig *config);

#endif // GAME_HPP
