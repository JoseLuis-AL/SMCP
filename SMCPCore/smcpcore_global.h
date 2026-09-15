#pragma once

#include <QtCore/qglobal.h>

#ifndef BUILD_STATIC
# ifdef SMCPCORE_LIB
#  define SMCPCORE_EXPORT Q_DECL_EXPORT
# else
#  define SMCPCORE_EXPORT Q_DECL_IMPORT
# endif
#else
# define SMCPCORE_EXPORT
#endif
