#include "kvcache/block_table.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace kvcache;

void print(const char* name, const BlockTable& table) {
    std::cout << name << " blocks=[";
    for (std::size_t i = 0; i < table.blocks().size(); ++i) {
        if (i) std::cout << ',';
        std::cout << table.blocks()[i];
    }
    std::cout << "] values=[";
    for (std::size_t i = 0; i < table.token_count(); ++i) {
        if (i) std::cout << ',';
        std::cout << static_cast<unsigned>(table.read_token(i).front());
    }
    std::cout << "]\n";
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: block_demo <input-file>");
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("cannot open input file");
        int count;
        if (!(input >> count) || count < 0 || count > 7)
            throw std::invalid_argument("expected a prompt length from 0 to 7");
        BlockPool pool(4, 2, 1);
        BlockTable parent(pool);
        for (int i = 0; i < count; ++i) {
            int value;
            if (!(input >> value) || value < 0 || value > 255)
                throw std::invalid_argument("expected a byte value from 0 to 255");
            if (!parent.append({static_cast<std::uint8_t>(value)}))
                throw std::runtime_error("capacity exhausted");
        }
        int extra;
        if (!(input >> extra) || extra < 0 || extra > 255)
            throw std::invalid_argument("expected one append byte");
        std::string unexpected;
        if (input >> unexpected) throw std::invalid_argument("unexpected extra input");
        print("parent", parent);
        auto child = parent.fork();
        print("fork", child);
        bool ok = child.append({static_cast<std::uint8_t>(extra)});
        std::cout << "append=" << (ok ? "OK" : "NO_CAPACITY")
                  << " copies=" << child.copy_count() << '\n';
        print("parent", parent);
        print("child", child);
        parent.clear();
        std::cout << "after parent release: free=" << pool.free_count() << '\n';
        print("child", child);
        child.clear();
        std::cout << "after child release: free=" << pool.free_count() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
