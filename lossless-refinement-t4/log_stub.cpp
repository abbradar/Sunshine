#include <boost/log/common.hpp>
boost::log::sources::severity_logger<int> verbose(boost::log::keywords::severity = 0);
boost::log::sources::severity_logger<int> debug(boost::log::keywords::severity = 1);
boost::log::sources::severity_logger<int> info(boost::log::keywords::severity = 2);
boost::log::sources::severity_logger<int> warning(boost::log::keywords::severity = 3);
boost::log::sources::severity_logger<int> error(boost::log::keywords::severity = 4);
boost::log::sources::severity_logger<int> fatal(boost::log::keywords::severity = 5);
boost::log::sources::severity_logger<int> tests(boost::log::keywords::severity = 6);
