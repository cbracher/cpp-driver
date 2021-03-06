
/*
 * File:   cql_query_validation_exception.hpp
 * Author: mc
 *
 * Created on September 16, 2013, 9:53 AM
 */

#ifndef CQL_QUERY_VALIDATION_EXCEPTION_H_
#define	CQL_QUERY_VALIDATION_EXCEPTION_H_

#include <string>
#include "cql/exceptions/cql_exception.hpp"

namespace cql {
/**
 * An exception indicating that a query cannot be executed because it is
 * incorrect syntactically, invalid, unauthorized or any other reason.
 */
class CQL_EXPORT cql_query_validation_exception: public cql_exception {
protected:
    cql_query_validation_exception(const char* message)
        : cql_exception(message) { }

	cql_query_validation_exception(const std::string& message)
		: cql_exception(message) { }
};
}

#endif	/* CQL_QUERY_VALIDATION_EXCEPTION_H_ */
