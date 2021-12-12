#pragma once

namespace Common {
// random hash combign for ints from from the internets.
// https://stackoverflow.com/questions/20511347/a-good-hash-function-for-a-vector
uint32_t ComputeChecksum(std::vector<uint32_t> const& vec)
{
    uint32_t seed = vec.size();
    for (auto& i : vec) {
        seed ^= i + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
}

const char* GetArg(const char* tag, int argc, const char** argv)
{
    auto i = find_if(argv, argv + argc,
                     [tag](auto to_test) { return 0 == strcmp(tag, to_test); });
    if (i == argv + argc)
        return nullptr;
    return i[1]; // return the value of the arg
}

int GetIntArg(const char* tag, int argc, const char** argv, int default_value)
{
    auto i = GetArg(tag, argc, argv);
    return i ? stoi(i) : default_value;
}

bool HasArg(const char* tag, int argc, const char** argv)
{
    auto i = find_if(argv, argv + argc,
                     [tag](auto to_test) { return 0 == strcmp(tag, to_test); });
    return (i != argv + argc);
}

string RandomUUID(int len)
{
    static random_device dev;
    static mt19937 rng(dev());
    static const char alphabet[] = "0123456789"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz";

    uniform_int_distribution<int> dist(0, size(alphabet) - 1);

    string res(len, 0);
    generate(begin(res), end(res), [&]() { return alphabet[dist(rng)]; });
    return res;
}
} // namespace Common