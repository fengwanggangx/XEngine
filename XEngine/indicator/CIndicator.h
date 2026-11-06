#ifndef XENGINE_INDICATOR_CINDICATOR_H
#define XENGINE_INDICATOR_CINDICATOR_H

#include <cstddef>
#include <span>
#include <string>

class CIndicator final
{
public:
	CIndicator() = default;
	~CIndicator();
	CIndicator(const CIndicator& arg) = delete;
	CIndicator& operator=(const CIndicator& arg) = delete;

	void Finalize();
	bool Initialize(std::string& error);
	bool CalculateSma(std::span<const double> prices, std::size_t period, double& result, std::string& error) const;
};

#endif
