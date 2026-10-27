// C++ macros for generating OTIO shim functions with minimal boilerplate.
// These macros ensure consistent error handling and reduce repetitive code.

#ifndef OTIO_SHIM_MACROS_H
#define OTIO_SHIM_MACROS_H

#include <cstring>

// ============================================================================
// String Accessor Macros
// ============================================================================

// Generate a string getter function: char* otio_<ctype>_get_<method>(Otio<Type>* obj)
// Returns malloc'd string that caller must free with otio_free_string
#define OTIO_STRING_GETTER(Type, ctype, method) \
    char* otio_##ctype##_get_##method(Otio##Type* obj) { \
        if (!obj) return nullptr; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            return safe_strdup(typed->method()); \
        } catch (...) { \
            return nullptr; \
        } \
    }

// Generate a string setter function: void otio_<ctype>_set_<method>(Otio<Type>* obj, const char* value)
#define OTIO_STRING_SETTER(Type, ctype, method) \
    void otio_##ctype##_set_##method(Otio##Type* obj, const char* value) { \
        if (!obj || !value) return; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            typed->set_##method(std::string(value)); \
        } catch (...) { \
        } \
    }

// ============================================================================
// TimeRange Accessor Macros
// ============================================================================

// Generate a TimeRange getter: OtioTimeRange otio_<ctype>_get_<method>(Otio<Type>* obj)
#define OTIO_TIME_RANGE_GETTER(Type, ctype, method) \
    OtioTimeRange otio_##ctype##_get_##method(Otio##Type* obj) { \
        OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}}; \
        if (!obj) return zero; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            auto range = typed->method(); \
            return OtioTimeRange{ \
                OtioRationalTime{range.start_time().value(), range.start_time().rate()}, \
                OtioRationalTime{range.duration().value(), range.duration().rate()} \
            }; \
        } catch (...) { \
            return zero; \
        } \
    }

// Generate an optional TimeRange getter (for types that return std::optional<TimeRange>)
#define OTIO_OPTIONAL_TIME_RANGE_GETTER(Type, ctype, method) \
    OtioTimeRange otio_##ctype##_get_##method(Otio##Type* obj) { \
        OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}}; \
        if (!obj) return zero; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            auto opt_range = typed->method(); \
            if (opt_range.has_value()) { \
                auto& range = opt_range.value(); \
                return OtioTimeRange{ \
                    OtioRationalTime{range.start_time().value(), range.start_time().rate()}, \
                    OtioRationalTime{range.duration().value(), range.duration().rate()} \
                }; \
            } \
            return zero; \
        } catch (...) { \
            return zero; \
        } \
    }

// Generate a TimeRange setter with error: int otio_<ctype>_set_<method>(Otio<Type>* obj, OtioTimeRange range, OtioError* err)
#define OTIO_TIME_RANGE_SETTER(Type, ctype, method) \
    int otio_##ctype##_set_##method(Otio##Type* obj, OtioTimeRange range, OtioError* err) { \
        if (!obj) { \
            set_error(err, 1, #Type " is null"); \
            return -1; \
        } \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            typed->set_##method(to_otio_tr(range)); \
            return 0; \
        } catch (const std::exception& e) { \
            set_error(err, 1, e.what()); \
            return -1; \
        } catch (...) { \
            set_error(err, 1, "Unknown exception"); \
            return -1; \
        } \
    }

// ============================================================================
// RationalTime Accessor Macros
// ============================================================================

// Generate a RationalTime getter: OtioRationalTime otio_<ctype>_get_<method>(Otio<Type>* obj)
#define OTIO_RATIONAL_TIME_GETTER(Type, ctype, method) \
    OtioRationalTime otio_##ctype##_get_##method(Otio##Type* obj) { \
        OtioRationalTime zero = {0, 1}; \
        if (!obj) return zero; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            auto rt = typed->method(); \
            return OtioRationalTime{rt.value(), rt.rate()}; \
        } catch (...) { \
            return zero; \
        } \
    }

// Generate a RationalTime setter: void otio_<ctype>_set_<method>(Otio<Type>* obj, OtioRationalTime time)
#define OTIO_RATIONAL_TIME_SETTER(Type, ctype, method) \
    void otio_##ctype##_set_##method(Otio##Type* obj, OtioRationalTime time) { \
        if (!obj) return; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            typed->set_##method(to_otio_rt(time)); \
        } catch (...) { \
        } \
    }

// ============================================================================
// Boolean Accessor Macros
// ============================================================================

// Generate a bool getter: int otio_<ctype>_get_<method>(Otio<Type>* obj) -> 0/1/-1
#define OTIO_BOOL_GETTER(Type, ctype, method) \
    int otio_##ctype##_get_##method(Otio##Type* obj) { \
        if (!obj) return -1; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            return typed->method() ? 1 : 0; \
        } catch (...) { \
            return -1; \
        } \
    }

// Generate a bool setter: void otio_<ctype>_set_<method>(Otio<Type>* obj, int value)
#define OTIO_BOOL_SETTER(Type, ctype, method) \
    void otio_##ctype##_set_##method(Otio##Type* obj, int value) { \
        if (!obj) return; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            typed->set_##method(value != 0); \
        } catch (...) { \
        } \
    }

// ============================================================================
// Double Accessor Macros
// ============================================================================

// Generate a double getter: double otio_<ctype>_get_<method>(Otio<Type>* obj)
#define OTIO_DOUBLE_GETTER(Type, ctype, method) \
    double otio_##ctype##_get_##method(Otio##Type* obj) { \
        if (!obj) return 0.0; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            return typed->method(); \
        } catch (...) { \
            return 0.0; \
        } \
    }

// Generate a double setter: void otio_<ctype>_set_<method>(Otio<Type>* obj, double value)
#define OTIO_DOUBLE_SETTER(Type, ctype, method) \
    void otio_##ctype##_set_##method(Otio##Type* obj, double value) { \
        if (!obj) return; \
        try { \
            auto typed = reinterpret_cast<otio::Type*>(obj); \
            typed->set_##method(value); \
        } catch (...) { \
        } \
    }

// ============================================================================
// Metadata Macros
// ============================================================================

// Generate both metadata getter and setter for a type
#define OTIO_METADATA_IMPL(Type, ctype) \
    void otio_##ctype##_set_metadata_string(Otio##Type* obj, const char* key, const char* value) { \
        set_metadata_string_impl(reinterpret_cast<otio::Type*>(obj), key, value); \
    } \
    char* otio_##ctype##_get_metadata_string(Otio##Type* obj, const char* key) { \
        return get_metadata_string_impl(reinterpret_cast<otio::Type*>(obj), key); \
    }

#endif // OTIO_SHIM_MACROS_H
