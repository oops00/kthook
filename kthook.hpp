#ifndef KTHOOK_HPP
#define KTHOOK_HPP
#include "xbyak/xbyak.h"
#include "ktsignal.hpp"
#include <memory>
#include <type_traits>
#include <cstdint>
#include <cstddef>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <asm/cachectl.h>
#endif

namespace kthook {
#ifdef XBYAK64
#include "./hde/hde64.h"
    using hde = hde64s;
#define hde_disasm(code, hs) hde64_disasm(code, hs)
#else
#include "./hde/hde32.h"
    using hde = hde32s;
#define hde_disasm(code, hs) hde32_disasm(code, hs)
#endif

    // from https://github.com/TsudaKageyu/minhook/blob/master/src/trampoline.h
#pragma pack(push, 1)
#ifdef XBYAK64
    struct JCC_ABS
    {
        std::uint8_t  opcode;      // 7* 0E:         J** +16
        std::uint8_t  dummy0;
        std::uint8_t  dummy1;      // FF25 00000000: JMP [+6]
        std::uint8_t  dummy2;
        std::uint32_t dummy3;
        std::uint64_t address;     // Absolute destination address
    };

    struct CALL_ABS
    {
        std::uint8_t  opcode0;     // FF15 00000002: CALL [+6]
        std::uint8_t  opcode1;
        std::uint32_t dummy0;
        std::uint8_t  dummy1;      // EB 08:         JMP +10
        std::uint8_t  dummy2;
        std::uint64_t address;     // Absolute destination address
    };

    struct JMP_ABS
    {
        std::uint8_t  opcode0;     // FF25 00000000: JMP [+6]
        std::uint8_t  opcode1;
        std::uint32_t dummy;
        std::uint64_t address;     // Absolute destination address
    };
#else
    typedef struct
    {
        std::uint8_t  opcode;      // E9/E8 xxxxxxxx: JMP/CALL +5+xxxxxxxx
        std::uint32_t operand;     // Relative destination address
    } JMP_REL, CALL_REL;

    struct JCC_REL
    {
        std::uint8_t  opcode0;     // 0F8* xxxxxxxx: J** +6+xxxxxxxx
        std::uint8_t  opcode1;
        std::uint32_t operand;     // Relative destination address
    };
#endif
#pragma pack(pop)

    namespace detail {
        std::size_t detect_hook_size(std::uintptr_t addr) {
            size_t size = 0;
            while (size < 5) {
                hde op;
                hde_disasm(reinterpret_cast<void*>(addr), &op);
                size += op.len;
                addr += op.len;
            }
            return size;
        }

        constexpr std::uintptr_t get_relative_address(std::uintptr_t dest, std::uintptr_t src, std::size_t oplen = 5) { return dest - src - oplen; }
        constexpr std::uintptr_t restore_absolute_address(std::uintptr_t RIP, std::uintptr_t rel, std::size_t oplen = 5) { return RIP + rel + oplen; }

        bool flush_intruction_cache(void* ptr, std::size_t size) {
#ifdef _WIN32
            return FlushInstructionCache(GetCurrentProcess(), ptr, size) != 0;
#else
            return cacheflush(ptr, size, ICACHE) == 0;
#endif
        }

        bool check_is_executable(void* addr) {
#ifdef _WIN32
            MEMORY_BASIC_INFORMATION buffer;
            VirtualQuery(addr, &buffer, sizeof(buffer));
            return buffer.Protect == PAGE_EXECUTE || buffer.Protect == PAGE_EXECUTE_READ || PAGE_EXECUTE_READWRITE;
#else
            return true;
#endif
        }

        enum class MemoryProt {
            PROTECT_RW,
            PROTECT_RWE,
            PROTECT_RE,
        };

        bool set_memory_prot(const void* addr, std::size_t size, MemoryProt protectMode)
        {
#if defined(_WIN32)
            const DWORD c_rw = PAGE_READWRITE;
            const DWORD c_rwe = PAGE_EXECUTE_READWRITE;
            const DWORD c_re = PAGE_EXECUTE_READ;
            DWORD mode;
#else
            const int c_rw = PROT_READ | PROT_WRITE;
            const int c_rwe = PROT_READ | PROT_WRITE | PROT_EXEC;
            const int c_re = PROT_READ | PROT_EXEC;
            int mode;
#endif
            switch (protectMode) {
            case MemoryProt::PROTECT_RW: mode = c_rw; break;
            case MemoryProt::PROTECT_RWE: mode = c_rwe; break;
            case MemoryProt::PROTECT_RE: mode = c_re; break;
            default:
                return false;
            }
#if defined(_WIN32)
            DWORD oldProtect;
            return VirtualProtect(const_cast<void*>(addr), size, mode, &oldProtect) != 0;
#elif defined(__GNUC__)
            size_t pageSize = sysconf(_SC_PAGESIZE);
            size_t iaddr = reinterpret_cast<size_t>(addr);
            size_t roundAddr = iaddr & ~(pageSize - static_cast<size_t>(1));
            return mprotect(reinterpret_cast<void*>(roundAddr), size + (iaddr - roundAddr), mode) == 0;
#else
            return true;
#endif
        }
    }

    enum class cconv {
        ccdecl,
        cfastcall,
        cthiscall,
        cstdcall,
    };

    namespace func_type_traits
    {
        template <typename>
        struct function_convention {};
        template <typename Ret, typename... Args>
        struct function_convention<Ret(__stdcall*) (Args...)>
        {
            static constexpr cconv value = cconv::cstdcall;
        };
        template <typename Ret, typename... Args>
        struct function_convention<Ret(__cdecl*) (Args...)>
        {
            static constexpr cconv value = cconv::ccdecl;
        };
        template <typename Ret, typename Class, typename... Args>
        struct function_convention<Ret(Class::*)(Args...)>
        {
            static constexpr cconv value = cconv::cthiscall;
        };
        template <typename Ret, typename... Args>
        struct function_convention<Ret(__fastcall*) (Args...)>
        {
            static constexpr cconv value = cconv::cfastcall;
        };
        template <typename Ret, typename... Args>
        struct function_convention<Ret(__thiscall*) (Args...)>
        {
            static constexpr cconv value = cconv::cthiscall;
        };
        template <typename Func>
        constexpr cconv function_convention_v = function_convention<Func>::value;
    };

#ifdef XBYAK32
#ifdef __GNUC__
#define CCDECL __attribute__((cdecl))
#define CFASTCALL __attribute__((fastcall))
#define CSTDCALL __attribute__((stdcall))
#define CTHISCALL __attribute__((thiscall))
#else
#define CCDECL __cdecl
#define CFASTCALL __fastcall
#define CSTDCALL __stdcall
#define CTHISCALL __thiscall
#endif

    template <typename HookType, cconv Convention, typename Ret, typename... Args>
    struct relay_generator;

    template <typename HookType, typename Ret, typename... Args>
    struct relay_generator<HookType, cconv::cstdcall, Ret, Args...> {
        static Ret CSTDCALL relay(HookType* this_hook, Args... args) {
            using SourceType = Ret(CSTDCALL*)(Args...);

            auto before_iterate = this_hook->onBefore.emit_iterate(args...);
            bool dont_skip_original = true;
            for (bool return_value : before_iterate) {
                dont_skip_original &= return_value;
            }
            if (dont_skip_original) {
                if constexpr (std::is_void_v<Ret>) {
                    reinterpret_cast<SourceType>(this_hook->trampoline)(args...);
                    this_hook->onAfter.emit(args...);
                    return;
                }
                else {
                    Ret return_value{ std::move(this_hook->trampoline(args...)) };
                    this_hook->onAfter.emit(&return_value, args...);
                    return return_value;
                }
            }
            if constexpr (!std::is_void_v<Ret>)
                return Ret{};
        }
    };

    template <typename HookType, typename Ret, typename... Args>
    struct relay_generator<HookType, cconv::cthiscall, Ret, Args...> {
        static Ret CSTDCALL relay(HookType* this_hook, Args... args) {
            using SourceType = Ret(CTHISCALL*)(Args...);

            auto before_iterate = this_hook->onBefore.emit_iterate(args...);
            bool dont_skip_original = true;
            for (bool return_value : before_iterate) {
                dont_skip_original &= return_value;
            }
            if (dont_skip_original) {
                if constexpr (std::is_void_v<Ret>) {
                    reinterpret_cast<SourceType>(this_hook->trampoline)(args...);
                    this_hook->onAfter.emit(args...);
                    return;
                }
                else {
                    Ret return_value{ std::move(this_hook->trampoline(args...)) };
                    this_hook->onAfter.emit(&return_value, args...);
                    return return_value;
                }
            }
            if constexpr (!std::is_void_v<Ret>)
                return Ret{};
        }
    };

    template <typename HookType, typename Ret, typename... Args>
    struct relay_generator<HookType, cconv::cfastcall, Ret, Args...> {

        // fastcall uses registers for first two integral/pointer types (left->right)
        // so we can use this trick to get outr hook object from stack
        struct fastcall_trick {
            HookType* ptr;
        };

        static Ret CFASTCALL relay(fastcall_trick esp4, Args... args) {
            using SourceType = Ret(CFASTCALL*)(Args...);

            HookType* this_hook = esp4.ptr;
            auto before_iterate = this_hook->onBefore.emit_iterate(args...);
            bool dont_skip_original = true;
            for (bool return_value : before_iterate) {
                dont_skip_original &= return_value;
            }
            if (dont_skip_original) {
                if constexpr (std::is_void_v<Ret>) {
                    reinterpret_cast<SourceType>(this_hook->trampoline)(args...);
                    this_hook->onAfter.emit(args...);
                    return;
                }
                else {
                    Ret return_value{ std::move(this_hook->trampoline(args...)) };
                    this_hook->onAfter.emit(&return_value, args...);
                    return return_value;
                }
            }
            if constexpr (!std::is_void_v<Ret>)
                return Ret{};
        }
    };

    template <typename HookType, typename Ret, typename... Args>
    struct relay_generator<HookType, cconv::ccdecl, Ret, Args...> {
        static Ret CCDECL relay(HookType* this_hook, std::uintptr_t retaddr, Args... args) {
            using SourceType = Ret(CCDECL*)(Args...);

            auto before_iterate = this_hook->onBefore.emit_iterate(args...);
            bool dont_skip_original = true;
            for (bool return_value : before_iterate) {
                dont_skip_original &= return_value;
            }
            if (dont_skip_original) {
                if constexpr (std::is_void_v<Ret>) {
                    reinterpret_cast<SourceType>(this_hook->trampoline)(args...);
                    this_hook->onAfter.emit(args...);
                    return;
                }
                else {
                    Ret return_value{ std::move(this_hook->trampoline(args...)) };
                    this_hook->onAfter.emit(&return_value, args...);
                    return return_value;
                }
            }
            if constexpr (!std::is_void_v<Ret>)
                return Ret{};
        }
    };
#endif
    template <auto* dest, cconv Convention, typename Ret, typename... Args>
    class kthook_impl {
        friend struct relay_generator<kthook_impl, Convention, Ret, Args...>;
        template <typename FuncSig>
        class hook_signal : public ktsignal::ktsignal_threadsafe<FuncSig> {
            using ktsignal::ktsignal_threadsafe<FuncSig>::emit;
            using ktsignal::ktsignal_threadsafe<FuncSig>::emit_iterate;
            friend struct relay_generator<kthook_impl, Convention, Ret, Args...>;
        };

        template<class T, class Enable = void>
        struct on_after_type {
            using type = hook_signal<void(std::add_lvalue_reference_t<Args>...)>;
        };

        template<class T>
        struct on_after_type<T, typename std::enable_if<!std::is_void_v<T>>::type> {
            using type = hook_signal<void(Ret&, std::add_lvalue_reference_t<Args>...)>;
        };

        using on_after_type_t = on_after_type<Ret>::type;
    public:
        hook_signal<bool(std::add_lvalue_reference_t<Args>...)> onBefore;

        on_after_type_t onAfter;

#ifdef XBYAK32
        kthook_impl(bool force_install = true) {
            hook_address = reinterpret_cast<std::uintptr_t>(dest);
            trampoline_gen = std::make_unique<Xbyak::CodeGenerator>();
            jump_gen = std::make_unique<Xbyak::CodeGenerator>();
            if (force_install)
                install();
        }
#else
        // WIP
#endif

        ~kthook_impl() {

        }

        bool install() {
            using namespace Xbyak::util;

            if (!detail::check_is_executable(reinterpret_cast<void*>(hook_address))) return false;

            if (!create_trampoline()) return false;
            detail::flush_intruction_cache(reinterpret_cast<void*>(trampoline), trampoline_gen->getSize());
            trampoline = const_cast<std::uint8_t*>(trampoline_gen->getCode());
            if (!patch_hook(true)) return false;
            return true;
        }

        bool remove() {
            if (!patch_hook(false)) return false;
            trampoline_gen->reset();
            jump_gen->reset();
            return true;
        }
        
    private:
        const std::uint8_t* generate_relay_jump() {
            using namespace Xbyak::util;
            if constexpr (Convention != cconv::ccdecl) {
                jump_gen->pop(eax);
            }
            if constexpr (Convention == cconv::cthiscall) {
                jump_gen->pop(ecx);
            }
            jump_gen->push(reinterpret_cast<std::uintptr_t>(this));
            if constexpr (Convention == cconv::ccdecl) {
                jump_gen->call(&relay_generator<kthook_impl, Convention, Ret, Args...>::relay);
                jump_gen->add(esp, 4);
                jump_gen->ret();
            }
            else {
                jump_gen->push(eax);
                jump_gen->jmp(&relay_generator<kthook_impl, Convention, Ret, Args...>::relay);
            }
            detail::flush_intruction_cache(reinterpret_cast<void*>(const_cast<std::uint8_t*>(jump_gen->getCode())), jump_gen->getSize());
            return jump_gen->getCode();
        }

        bool patch_hook(bool enable) {
            if (enable) {
                hook_size = detail::detect_hook_size(hook_address);
                original_code = std::make_unique<unsigned char[]>(hook_size);
                std::memcpy(original_code.get(), reinterpret_cast<void*>(hook_address), hook_size);

                if (!detail::set_memory_prot(reinterpret_cast<void*>(hook_address), hook_size, detail::MemoryProt::PROTECT_RWE)) return false;
                if (*reinterpret_cast<std::uint8_t*>(hook_address) == 0xE8) {
                    uintptr_t relative = detail::get_relative_address(reinterpret_cast<uintptr_t>(generate_relay_jump()), hook_address);
                    *reinterpret_cast<std::uint32_t*>(hook_address + 1) = relative;
                }
                else {
                    uintptr_t relative = detail::get_relative_address(reinterpret_cast<uintptr_t>(generate_relay_jump()), hook_address);
                    *reinterpret_cast<std::uint8_t*>(hook_address) = 0xE9;
                    *reinterpret_cast<std::uint32_t*>(hook_address + 1) = relative;
                    memset(reinterpret_cast<void*>(hook_address + 5), 0x90, hook_size - 5);
                }
                if (!detail::set_memory_prot(reinterpret_cast<void*>(hook_address), hook_size, detail::MemoryProt::PROTECT_RE)) return false;
            }
            else {
                if (!detail::set_memory_prot(reinterpret_cast<void*>(hook_address), hook_size, detail::MemoryProt::PROTECT_RWE)) return false;
                std::memcpy(reinterpret_cast<void*>(hook_address), original_code.get(), hook_size);
                if (!detail::set_memory_prot(reinterpret_cast<void*>(hook_address), hook_size, detail::MemoryProt::PROTECT_RE)) return false;
            }
            detail::flush_intruction_cache(reinterpret_cast<void*>(hook_address), hook_size);
            return true;
        }

        bool create_trampoline() {
            // save original code
#ifdef XBYAK64
            CALL_ABS call = {
                0xFF, 0x15, 0x00000002, // FF15 00000002: CALL [RIP+8]
                0xEB, 0x08,             // EB 08:         JMP +10
                0x0000000000000000ULL   // Absolute destination address
            };
            JMP_ABS jmp = {
                0xFF, 0x25, 0x00000000, // FF25 00000000: JMP [RIP+6]
                0x0000000000000000ULL   // Absolute destination address
            };
            JCC_ABS jcc = {
                0x70, 0x0E,             // 7* 0E:         J** +16
                0xFF, 0x25, 0x00000000, // FF25 00000000: JMP [RIP+6]
                0x0000000000000000ULL   // Absolute destination address
            };
#else
            CALL_REL call = {
                0xE8,                   // E8 xxxxxxxx: CALL +5+xxxxxxxx
                0x00000000              // Relative destination address
            };
            JMP_REL jmp = {
                0xE9,                   // E9 xxxxxxxx: JMP +5+xxxxxxxx
                0x00000000              // Relative destination address
            };
            JCC_REL jcc = {
                0x0F, 0x80,             // 0F8* xxxxxxxx: J** +6+xxxxxxxx
                0x00000000              // Relative destination address
            };
#endif
            std::size_t trampoline_size = 0;
            std::size_t op_copy_size = 0;
            void* op_copy_src = nullptr;
            std::uintptr_t current_address = hook_address;
            std::uintptr_t max_jmp_ref = 0;
            bool finished = false;
#ifdef XBYAK64
            std::uint8_t inst_buf[16];
#endif

            while (!finished) {
                hde hs;
                std::size_t op_copy_size = hde_disasm(reinterpret_cast<void*>(current_address), &hs);
                if (hs.flags & F_ERROR)
                    return false;
                op_copy_src = reinterpret_cast<void*>(current_address);
                if (current_address - hook_address >= sizeof(JMP_REL)) {
                    trampoline_gen->jmp(reinterpret_cast<std::uint8_t*>(current_address));
                    break;
                }
#ifdef XBYAK64
                else if ((hs.modrm & 0xC7) == 0x05)
                {
                    // Instructions using RIP relative addressing. (ModR/M = 00???101B)

                    // Modify the RIP relative address.
                    std::uint32_t* pRelAddr;

                    std::memcpy(inst_buf, reinterpret_cast<void*>(current_address), op_copy_size);

                    op_copy_src = inst_buf;

                    // Relative address is stored at (instruction length - immediate value length - 4).
                    pRelAddr = reinterpret_cast<std::uint32_t*>(inst_buf + hs.len - ((hs.flags & 0x3C) >> 2) - 4);
                    *pRelAddr = reinterpret_cast<std::uint32_t>((current_address + hs.len + hs.disp.disp32) - (trampoline_gen->getCurr() + trampoline_size + hs.len));

                    // Complete the function if JMP (FF /4).
                    if (hs.opcode == 0xFF && hs.modrm_reg == 4)
                        finished = true;
                }
#endif
                // Relative Call
                else if (hs.opcode == 0xE8)
                {
                    std::uintptr_t call_destination = detail::restore_absolute_address(current_address, hs.imm.imm32, hs.len);
#if XBYAK64
                    call.address = call_destination;
#else
                    call.operand = detail::get_relative_address(call_destination,
                        reinterpret_cast<std::uintptr_t>(trampoline_gen->getCurr() + trampoline_size), sizeof(call));
#endif
                    op_copy_src = &call;
                    op_copy_size = sizeof(call);
                }
                // Relative jmp
                else if ((hs.opcode & 0xFD) == 0xE9)
                {
                    std::uintptr_t jmp_destination = current_address + hs.len;

                    if (hs.opcode == 0xEB) // is short jump
                        jmp_destination += hs.imm.imm8;
                    else
                        jmp_destination += hs.imm.imm32;

                    if (hook_address <= jmp_destination
                        && jmp_destination < (hook_address + sizeof(JMP_REL)))
                    {
                        if (max_jmp_ref < jmp_destination)
                            max_jmp_ref = jmp_destination;
                    }
                    else
                    {
#if XBYAK64
                        jmp.address = jmp_destination;
#else
                        jmp.operand = detail::get_relative_address(jmp_destination,
                            reinterpret_cast<std::uintptr_t>(trampoline_gen->getCurr() + trampoline_size), sizeof(jmp));
#endif
                        op_copy_src = &jmp;
                        op_copy_size = sizeof(jmp);

                        // Exit the function if it is not in the branch.
                        finished = (hook_address >= max_jmp_ref);
                    }
                }
                // Conditional relative jmp
                else if (((hs.opcode & 0xF0) == 0x70) ||     // one byte jump
                    ((hs.opcode & 0xFC) == 0xE0) ||     // LOOPNZ/LOOPZ/LOOP/JECXZ
                    ((hs.opcode2 & 0xF0) == 0x80)) {    // two byte jump

                    std::uintptr_t jmp_destination = current_address + hs.len;

                    if ((hs.opcode & 0xF0) == 0x70      // Jcc
                        || (hs.opcode & 0xFC) == 0xE0)  // LOOPNZ/LOOPZ/LOOP/JECXZ
                        jmp_destination += hs.imm.imm8;
                    else
                        jmp_destination += hs.imm.imm32;

                    // Simply copy an internal jump.
                    if (hook_address <= jmp_destination
                        && jmp_destination < (hook_address + sizeof(JMP_REL)))
                    {
                        if (max_jmp_ref < jmp_destination)
                            max_jmp_ref = jmp_destination;
                    }
                    else if ((hs.opcode & 0xFC) == 0xE0)
                    {
                        // LOOPNZ/LOOPZ/LOOP/JCXZ/JECXZ to the outside are not supported.
                        return false;
                    }
                    else
                    {
                        std::uint8_t cond = ((hs.opcode != 0x0F ? hs.opcode : hs.opcode2) & 0x0F);
#if XBYAK64
                        // Invert the condition in x64 mode to simplify the conditional jump logic.
                        jcc.opcode = 0x71 ^ cond;
                        jcc.address = dest;
#else
                        jcc.opcode1 = 0x80 | cond;
                        jcc.operand = detail::get_relative_address(jmp_destination,
                            reinterpret_cast<std::uintptr_t>(trampoline_gen->getCurr() + trampoline_size), sizeof(jcc));
#endif
                        op_copy_src = &jcc;
                        op_copy_size = sizeof(jcc);
                    }
                }
                // RET
                else if ((hs.opcode & 0xFE) == 0xC2)
                {
                    finished = (current_address >= max_jmp_ref);
                }

                trampoline_gen->db(reinterpret_cast<std::uint8_t*>(op_copy_src), op_copy_size);

                trampoline_size += op_copy_size;
                current_address += hs.len;

            }

            if (current_address - hook_address < sizeof(JMP_REL))
                return false;
            return true;
        }

        std::uintptr_t hook_address;
        std::size_t hook_size;
        std::unique_ptr<unsigned char[]> original_code;
        std::unique_ptr<Xbyak::CodeGenerator> trampoline_gen;
        std::unique_ptr<Xbyak::CodeGenerator> jump_gen;
        std::uint8_t* trampoline;
#ifdef XBYAK32
        cconv hook_convention;
#endif
    };

    template <auto* f, typename = decltype(f)>
    struct kthook {};

    template <auto* f, typename Ret, typename... Args>
    struct kthook<f, Ret(CCDECL*)(Args...)> {
        using type = kthook_impl<f, func_type_traits::function_convention_v<decltype(f)>, Ret, Args...>;
    };

    template <auto* f, typename Ret, typename... Args>
    struct kthook<f, Ret(CSTDCALL*)(Args...)> {
        using type = kthook_impl<f, func_type_traits::function_convention_v<decltype(f)>, Ret, Args...>;
    };

    template <auto* f, typename Ret, typename... Args>
    struct kthook<f, Ret(CTHISCALL*)(Args...)> {
        using type = kthook_impl<f, func_type_traits::function_convention_v<decltype(f)>, Ret, Args...>;
    };

    template <auto* f, typename Ret, typename... Args>
    struct kthook<f, Ret(CFASTCALL*)(Args...)> {
        using type = kthook_impl<f, func_type_traits::function_convention_v<decltype(f)>, Ret, Args...>;
    };

    template <auto* f>
    using kthook_t = typename kthook<f>::type;
}
#endif // KTHOOK_HPP