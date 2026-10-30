#pragma once

#include <cstddef>

namespace sf {
    template<typename T>
    struct block {
        T *data = nullptr;
        size_t size = 0;

        explicit operator bool() const noexcept {
            return data != nullptr;
        }
    };
}