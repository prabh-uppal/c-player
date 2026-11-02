#pragma once

enum class K { None, Up, Down, Left, Right, Enter, Back, Esc, PgUp, PgDn, Char };

struct Key {
    K k = K::None;
    char c = 0;
};

Key readKey(int timeoutMs);
