//
// Created by MattFor on 13/12/2025.
//

#ifndef SO_PROJECT_PATIENT_H
#define SO_PROJECT_PATIENT_H

#include <mutex>
#include <string>
#include <atomic>
#include <optional>
#include <functional>

using LoggerFn = std::function <void(const std::string&)>;

struct PatientData
{
	std::string id;
	std::string name;

	bool vip = false;
	unsigned int age = 0;

	std::string symptoms;
};

class ILogger
{
public:
	virtual ~ILogger() = default;

	virtual void log(const std::string& msg) = 0;
};

class Patient
{
public:
	Patient(const PatientData& data, ILogger* loggerPtr = nullptr, LoggerFn loggerFn = nullptr);

	Patient(const Patient&) = delete;

	Patient& operator =(const Patient&) = delete;

	Patient(Patient&&) noexcept;

	Patient& operator =(Patient&&) noexcept;

	const PatientData& data() const noexcept;

	bool isChild() const noexcept; // age < 18

	bool validate() noexcept;

	std::string lastValidationError() const noexcept;

	void log(const std::string& msg) const noexcept;

	~Patient();

private:
	PatientData m_data;
	ILogger* m_loggerPtr = nullptr;
	LoggerFn m_loggerFn;
	mutable std::string m_lastValidationError;
	mutable std::mutex m_logMutex;

	void safeLog(const std::string& msg) const noexcept;
};

#endif //SO_PROJECT_PATIENT_H
