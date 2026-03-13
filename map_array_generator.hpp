#ifndef MAP_ARRAY_GENERATOR_HPP
#define MAP_ARRAY_GENERATOR_HPP

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

struct RoomTexture {
  std::string base;
  int id;
};

struct Room {
  int x, y, w, h;
  int room_type;
  RoomTexture texture;

  int center_x() const { return x + w / 2; }
  int center_y() const { return y + h / 2; }
};

class MapGenerator {
public:
  MapGenerator(int n) : N(n) {
    // Asignar memoria dinámica para el array bidimensional N x N
    matrix = new int *[N];
    for (int i = 0; i < N; i++) {
      matrix[i] = new int[N];
    }

    // Asignar memoria dinámica para el array unidimensional N * N
    flat_array = new int[N * N];

    // Inicializar ambos arrays en 0 (todo es pared sólida)
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < N; j++) {
        matrix[i][j] = 0;
        flat_array[i * N + j] = 0;
      }
    }

    srand(static_cast<unsigned>(time(nullptr)));
    generate_map();
  }

  ~MapGenerator() {
    for (int i = 0; i < N; i++) {
      delete[] matrix[i];
    }
    delete[] matrix;
    delete[] flat_array;
  }

  void print_matrix() {
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < N; j++) {
        std::cout << matrix[i][j] << " ";
      }
      std::cout << std::endl;
    }
  }

  int get_size() const { return N; }

  Room *rooms;
  int room_count;

  int **get_matrix() { return matrix; }

protected:
  int N;
  int **matrix;
  int *flat_array;

  // Parámetros de generación
  static const int ROOM_MIN_SIZE = 3;
  static const int ROOM_MAX_SIZE = 5;
  static const int MAX_ROOM_ATTEMPTS =
      50; // Intentos de generar salas principales
  static const int MAX_ROOMS =
      MAX_ROOM_ATTEMPTS * 3; // Capacidad total incluyendo pasillos

  int rand_range(int min, int max) { return min + (rand() % (max - min + 1)); }

  // Verifica si una sala cabe y no se superpone con otra existente
  bool can_place_room(const Room &room) {
    if (room.x < 1 || room.y < 1 || room.x + room.w >= N ||
        room.y + room.h >= N) {
      return false;
    }

    // Revisar el área de la sala + 1 celda de margen en cada dirección
    int start_row = (room.y - 1 > 0) ? room.y - 1 : 0;
    int end_row = (room.y + room.h + 1 < N) ? room.y + room.h + 1 : N;
    int start_col = (room.x - 1 > 0) ? room.x - 1 : 0;
    int end_col = (room.x + room.w + 1 < N) ? room.x + room.w + 1 : N;

    for (int row = start_row; row < end_row; row++) {
      for (int col = start_col; col < end_col; col++) {
        if (matrix[row][col] != 0) {
          return false;
        }
      }
    }
    return true;
  }

  // Esculpe una sala en el mapa (pone el room_type en el área de la sala)
  void carve_room(const Room &room) {
    for (int row = room.y; row < room.y + room.h; row++) {
      for (int col = room.x; col < room.x + room.w; col++) {
        matrix[row][col] = room.room_type;
      }
    }
  }

  // Cava un pasillo horizontal desde x1 hasta x2 en la fila y
  void carve_horizontal_corridor(int x1, int x2, int y,
                                 const RoomTexture &texture) {
    int start = (x1 < x2) ? x1 : x2;
    int end = (x1 < x2) ? x2 : x1;

    if (room_count < MAX_ROOMS) {
      rooms[room_count] = {start, y, end - start + 1, 1, 6, texture};
      room_count++;
    }

    for (int col = start; col <= end; col++) {
      if (y >= 0 && y < N && col >= 0 && col < N) {
        matrix[y][col] = 6; // 6 para pasillos
      }
    }
  }

  // Cava un pasillo vertical desde y1 hasta y2 en la columna x
  void carve_vertical_corridor(int y1, int y2, int x,
                               const RoomTexture &texture) {
    int start = (y1 < y2) ? y1 : y2;
    int end = (y1 < y2) ? y2 : y1;

    if (room_count < MAX_ROOMS) {
      rooms[room_count] = {x, start, 1, end - start + 1, 6, texture};
      room_count++;
    }

    for (int row = start; row <= end; row++) {
      if (row >= 0 && row < N && x >= 0 && x < N) {
        matrix[row][x] = 6; // 6 para pasillos
      }
    }
  }

  // Sincroniza la matriz 2D con el array unidimensional
  void sync_flat_array() {
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < N; j++) {
        flat_array[i * N + j] = matrix[i][j];
      }
    }
  }

  void generate_map() {
    rooms = new Room[MAX_ROOMS];
    room_count = 0;

    for (int attempt = 0; attempt < MAX_ROOM_ATTEMPTS; attempt++) {
      if (room_count >= MAX_ROOMS - 3) {
        // Evita exceder búfer al añadir la sala + pasillos
        break;
      }

      int w = rand_range(ROOM_MIN_SIZE, ROOM_MAX_SIZE);
      int h = rand_range(ROOM_MIN_SIZE, ROOM_MAX_SIZE);

      // Si la sala no cabe en el mapa, descartar este intento
      if (w + 2 > N || h + 2 > N) {
        continue;
      }

      int x = rand_range(1, N - w - 1);
      int y = rand_range(1, N - h - 1);

      int type = rand_range(1, 5);
      RoomTexture rtex;
      switch (type) {
      case 1:
        rtex.base = "Gem";
        break;
      case 2:
        rtex.base = "Metal";
        break;
      case 3:
        rtex.base = "Stone";
        break;
      case 4:
        rtex.base = "Terrain";
        break;
      case 5:
        rtex.base = "Weave";
        break;
      default:
        rtex.base = "Stone";
        break;
      }
      rtex.id = rand_range(1, 24);

      Room new_room = {x, y, w, h, type, rtex};

      if (!can_place_room(new_room)) {
        continue;
      }

      // Esculpir la sala
      carve_room(new_room);

      // Conectar con la sala anterior mediante pasillos en L
      if (room_count > 0) {
        int prev_cx = rooms[room_count - 1].center_x();
        int prev_cy = rooms[room_count - 1].center_y();
        int new_cx = new_room.center_x();
        int new_cy = new_room.center_y();

        // Primero horizontal, luego vertical
        carve_horizontal_corridor(prev_cx, new_cx, prev_cy, new_room.texture);
        carve_vertical_corridor(prev_cy, new_cy, new_cx, new_room.texture);
      }

      if (room_count < MAX_ROOMS) {
        rooms[room_count] = new_room;
        room_count++;
      }
    }

    // Sincronizar el array 1D con la matriz generada
    sync_flat_array();
  }
};

#endif // MAP_ARRAY_GENERATOR_HPP
