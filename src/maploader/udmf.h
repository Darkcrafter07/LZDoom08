#ifndef __P_UDMF_H
#define __P_UDMF_H

#include "sc_man.h"
#include "m_fixed.h"

int     GetUDMFInt(FLevelLocals *Level, int type, int index, FName key);
double  GetUDMFFloat(FLevelLocals *Level, int type, int index, FName key);
FString GetUDMFString(FLevelLocals *Level, int type, int index, FName key);

class UDMFParserBase
{
protected:
	FScanner sc;
	FName namespc = NAME_None;
	int namespace_bits;
	FString parsedString;
	bool BadCoordinates = false;

	void Skip();
	FName ParseKey(bool checkblock = false, bool *isblock = NULL);
	int CheckInt(FName key);
	double CheckFloat(FName key);
	double CheckCoordinate(FName key);
	DAngle CheckAngle(FName key);
	bool CheckBool(FName key);
	const char *CheckString(FName key);
	int MatchString(FName key, const char* const* strings, int defval);

	template<typename T>
	bool Flag(T &value, int mask, FName key)
	{
		if (CheckBool(key))
		{
			value |= mask;
			return true;
		}
		else
		{
			value &= ~mask;
			return false;
		}
	}

};

#define BLOCK_ID (ENamedName)-1

#endif