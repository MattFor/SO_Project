//
// Created by MattFor on 13.12.2025.
//

#include <sstream>
#include <iostream>

#include "patient/Patient.h"

static std::string make_creation_message(const PatientData& d)
{
	std::ostringstream ss;
	ss << "PATIENT CREATED: id=" << d.id << " name=\"" << d.name << "\" age=" << d.age
	   << " vip=" << (d.vip ? "true" : "false") << " symptoms=\"" << d.symptoms << "\"";

	return ss.str();
}

static std::string make_destruction_message(const PatientData& d)
{
	std::ostringstream ss;
	ss << "PATIENT DESTROYED: id=" << d.id << " name=\"" << d.name << "\"";

	return ss.str();
}

Patient::Patient(const PatientData& data, ILogger* loggerPtr, LoggerFn loggerFn)
		: m_data(data), m_loggerPtr(loggerPtr), m_loggerFn(std::move(loggerFn))
{
	// Validate early but allow construction even with invalid data (validate() can be called)
	validate();

	// Log creation
	safeLog(make_creation_message(m_data));
}

Patient::Patient(Patient&& other) noexcept
		: m_data(std::move(other.m_data)),
		  m_loggerPtr(other.m_loggerPtr),
		  m_loggerFn(std::move(other.m_loggerFn))
{
	other.m_loggerPtr = nullptr;
	other.m_loggerFn = nullptr;

	safeLog(make_creation_message(m_data));
}

Patient& Patient::operator =(Patient&& other) noexcept
{
	if (this != &other)
	{
		safeLog(make_destruction_message(m_data));
		m_data = std::move(other.m_data);
		m_loggerPtr = other.m_loggerPtr;
		m_loggerFn = std::move(other.m_loggerFn);
		other.m_loggerPtr = nullptr;
		other.m_loggerFn = nullptr;
		safeLog(make_creation_message(m_data));
	}

	return *this;
}

const PatientData& Patient::data() const noexcept
{
	return m_data;
}

bool Patient::isChild() const noexcept
{
	return m_data.age < 18u;
}

bool Patient::validate() noexcept
{
	if (m_data.id.empty())
	{
		m_lastValidationError = "id is empty";
		safeLog(std::string("PATIENT VALIDATION FAILED: ") + m_lastValidationError);
		return false;
	}

	if (m_data.name.empty())
	{
		m_lastValidationError = "name is empty";
		safeLog(std::string("PATIENT VALIDATION FAILED: ") + m_lastValidationError);
		return false;
	}

	if (m_data.age > 150)
	{
		m_lastValidationError = "age out of realistic range (0..150)";
		safeLog(std::string("PATIENT VALIDATION FAILED: ") + m_lastValidationError);
		return false;
	}

	if (m_data.symptoms.empty())
	{
		m_lastValidationError = "symptoms empty";
		safeLog(std::string("PATIENT VALIDATION FAILED: ") + m_lastValidationError);
		return false;
	}

	m_lastValidationError.clear();

	return true;
}

std::string Patient::lastValidationError() const noexcept
{
	return m_lastValidationError;
}

void Patient::safeLog(const std::string& msg) const noexcept
{
	std::lock_guard <std::mutex> lk(m_logMutex);

	try
	{
		if (m_loggerFn)
		{
			m_loggerFn(msg);
			return;
		}

		if (m_loggerPtr)
		{
			m_loggerPtr->log(msg);
			return;
		}

		std::cerr << msg << std::endl;
	}
	catch (...)
	{
		// Nothing
	}
}

void Patient::log(const std::string& msg) const noexcept
{
	safeLog(msg);
}

Patient::~Patient()
{
	safeLog(make_destruction_message(m_data));
}
