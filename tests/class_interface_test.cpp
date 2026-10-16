// ClassBuilder must emit an interfaces table: the configuration-phase handoff implements
// GameProtocols$Context and java.util.function.BiConsumer with generated classes, which only works
// when the generated class actually declares those interfaces.
#include "classfile.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Reader {
    const std::vector<u1>& bytes;
    size_t pos = 0;

    u1 u1At() { return bytes[pos++]; }
    u2 u2At() {
        u2 value = static_cast<u2>((bytes[pos] << 8) | bytes[pos + 1]);
        pos += 2;
        return value;
    }
    u4 u4At() {
        u4 value = (static_cast<u4>(bytes[pos]) << 24) | (static_cast<u4>(bytes[pos + 1]) << 16) |
                   (static_cast<u4>(bytes[pos + 2]) << 8) | static_cast<u4>(bytes[pos + 3]);
        pos += 4;
        return value;
    }
    void skip(size_t count) { pos += count; }
};

// Walks the constant pool and returns, for every CP index, the class name it denotes (Class and
// Utf8 entries are separate slots, so Class entries are resolved after the walk).
std::vector<std::string> constantPoolClassNames(Reader& reader, u2 count) {
    std::vector<std::string> utf8(count + 1);
    std::vector<u2> nameIndex(count + 1, 0); // Class entry -> Utf8 index
    for (u2 index = 1; index < count; ++index) {
        u1 tag = reader.u1At();
        switch (tag) {
        case CP_Utf8: {
            u2 length = reader.u2At();
            utf8[index] = std::string(reinterpret_cast<const char*>(&reader.bytes[reader.pos]), length);
            reader.skip(length);
            break;
        }
        case CP_Class:
            nameIndex[index] = reader.u2At();
            break;
        case CP_String:
        case 16: // MethodType
        case 19: // Module
        case 20: // Package
            reader.u2At();
            break;
        case 3:  // Integer
        case 4:  // Float
            reader.skip(4);
            break;
        case 5:  // Long
        case 6:  // Double
            reader.skip(8);
            ++index; // occupies two constant pool slots
            break;
        case CP_Fieldref:
        case CP_Methodref:
        case CP_InterfaceMethodref:
        case CP_NameAndType:
        case 17: // Dynamic
        case 18: // InvokeDynamic
            reader.skip(4);
            break;
        case 15: // MethodHandle
            reader.skip(3);
            break;
        default:
            std::fprintf(stderr, "FAIL: unexpected constant pool tag %u at index %u\n", tag, index);
            return {};
        }
    }
    std::vector<std::string> names(count + 1);
    for (u2 index = 1; index < count; ++index) {
        if (nameIndex[index] != 0 && nameIndex[index] < utf8.size())
            names[index] = utf8[nameIndex[index]];
    }
    return names;
}

struct Interfaces {
    bool valid = false;
    u2 major = 0;
    u2 access = 0;
    std::vector<std::string> names;
};

// Reads the header (access/this/super/interfaces) after walking the constant pool.
Interfaces readInterfaces(const std::vector<u1>& bytes) {
    Interfaces result;
    if (bytes.size() < 24 || bytes[0] != 0xCA || bytes[1] != 0xFE || bytes[2] != 0xBA || bytes[3] != 0xBE)
        return result;
    Reader reader{bytes, 4};
    reader.u2At(); // minor_version
    result.major = reader.u2At();
    u2 count = reader.u2At();
    std::vector<std::string> classNames = constantPoolClassNames(reader, count);
    if (classNames.size() < count + 1)
        return result;
    result.access = reader.u2At();
    reader.u2At(); // this_class
    reader.u2At(); // super_class
    u2 interfaces = reader.u2At();
    for (u2 i = 0; i < interfaces; ++i) {
        u2 classIndex = reader.u2At();
        if (classIndex >= classNames.size() || classNames[classIndex].empty()) {
            std::fprintf(stderr, "FAIL: interface index %u does not resolve to a class name\n", classIndex);
            return result;
        }
        result.names.push_back(classNames[classIndex]);
    }
    result.valid = true;
    return result;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}

} // namespace

int main() {
    const std::string contextInterface = "net/minecraft/network/protocol/game/GameProtocols$Context";
    ClassBuilder withInterfaces("proxy/gen/Context", "java/lang/Object", 52);
    withInterfaces.addInterface(contextInterface);
    withInterfaces.addInterface("java/util/function/BiConsumer");
    withInterfaces.addNativeMethod("hasInfiniteMaterials", "()Z", ACC_PUBLIC | ACC_NATIVE);
    std::vector<u1> bytes = withInterfaces.build();

    Interfaces parsed = readInterfaces(bytes);
    if (!expect(parsed.valid, "generated class header is readable") ||
        !expect(parsed.major == 52, "class file version stays 52 (Java 8)") ||
        !expect(parsed.names.size() == 2, "both interfaces are declared") ||
        !expect(parsed.names[0] == contextInterface, "first interface keeps its internal name") ||
        !expect(parsed.names[1] == "java/util/function/BiConsumer", "second interface keeps its internal name"))
        return 1;

    // Regression guard: a class without addInterface must still declare an empty table.
    ClassBuilder plain("proxy/gen/Thread", "java/lang/Thread", 52);
    plain.addNativeMethod("run", "()V", ACC_PUBLIC | ACC_NATIVE);
    Interfaces plainParsed = readInterfaces(plain.build());
    if (!expect(plainParsed.valid, "plain class header is readable") ||
        !expect(plainParsed.names.empty(), "class without addInterface declares no interfaces"))
        return 1;

    std::puts("PASS: generated classes declare the interfaces they implement");
    return 0;
}
