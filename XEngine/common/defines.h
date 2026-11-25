#ifndef __COMMON_DEFINES_H__
#define __COMMON_DEFINES_H__

#define RETURN_EMTPTY_IFNULLPTR(...) \
	if (nullptr == (__VA_ARGS__))    \
	{                                \
		return {};                   \
	}

#define RETURN_VALUE_IFNULLPTR(ptr, val) \
	if (nullptr == ptr)                  \
	{                                    \
		return val;                      \
	}

#define RETURN_IFNULLPTR(...)     \
	if (nullptr == (__VA_ARGS__)) \
	{                             \
		return;                   \
	}

#define IS_NOT_NULLPTR(...) (nullptr != (__VA_ARGS__))

#define IS_NULLPTR(...) (nullptr == (__VA_ARGS__))

#define DECLARE_DELETE_COPY_CONSTRUCT(ClassName)     \
private:                                             \
	ClassName(const ClassName&) = delete;            \
	ClassName(ClassName&&) = delete;                 \
	ClassName& operator=(const ClassName&) = delete; \
	ClassName& operator=(ClassName&&) = delete;

#define DECLARE_ONLY_CUSTOM_CONSTRUCT(ClassName)     \
public:                                              \
	ClassName();                                     \
	~ClassName();                                    \
                                                     \
private:                                             \
	ClassName(const ClassName&) = delete;            \
	ClassName(ClassName&&) = delete;                 \
	ClassName& operator=(const ClassName&) = delete; \
	ClassName& operator=(ClassName&&) = delete;

#define DECLARE_DEFAULT_COPY_CONSTRUCT(ClassName)     \
private:                                              \
	ClassName(const ClassName&) = default;            \
	ClassName(ClassName&&) = default;                 \
	ClassName& operator=(const ClassName&) = default; \
	ClassName& operator=(ClassName&&) = default;

#include "../thread/CThreadPool.h"
CThreadPool* GetThreadPool();

#define ThreadPoolPtr GetThreadPool()

#endif
