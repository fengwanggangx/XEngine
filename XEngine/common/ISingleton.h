#ifndef __ISINGLETON_H__
#define __ISINGLETON_H__
#include <utility>

#define DECLARE_SINGLE_DFAULT_DELETE(ClassName)      \
	friend class ISingleton<ClassName>;              \
                                                     \
private:                                             \
	explicit ClassName() = delete;                   \
	~ClassName();                                    \
	ClassName(const ClassName&) = delete;            \
	ClassName(ClassName&&) = delete;                 \
	ClassName& operator=(const ClassName&) = delete; \
	ClassName& operator=(ClassName&&) = delete;

#define DECLARE_SINGLE_DFAULT(ClassName)             \
	friend class ISingleton<ClassName>;              \
                                                     \
private:                                             \
	explicit ClassName();                            \
	~ClassName();                                    \
	ClassName(const ClassName&) = delete;            \
	ClassName(ClassName&&) = delete;                 \
	ClassName& operator=(const ClassName&) = delete; \
	ClassName& operator=(ClassName&&) = delete;

template <typename _Ty>
class ISingleton
{
public:
	template <typename... _TyArg>
	static _Ty* InstancePtr(_TyArg&&... args)
	{
		static _Ty* pInstance = new _Ty(std::forward<_TyArg>(args)...);
		return pInstance;
	}

	template <typename... _TyArg>
	static _Ty& InstanceRef(_TyArg&&... args)
	{
		return *InstancePtr(std::forward<_TyArg>(args)...);
	}

	// 禁止拷贝继承
protected:
	ISingleton() = default;
	~ISingleton() = default;

private:
	ISingleton(const ISingleton&) = delete;
	ISingleton(ISingleton&&) = delete;
	ISingleton& operator=(const ISingleton&) = delete;
	ISingleton& operator=(ISingleton&&) = delete;
};
#endif
