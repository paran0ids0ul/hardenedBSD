//===-- MIUtilMapIdToVariant.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

//++
// File:		MIUtilMapIdToVariant.cpp
//
// Overview:	CMIUtilMapIdToVariant implementation.
//
// Environment:	Compilers:	Visual C++ 12.
//							gcc (Ubuntu/Linaro 4.8.1-10ubuntu9) 4.8.1
//				Libraries:	See MIReadmetxt. 
//
// Copyright:	None.
//--

// In-house headers:
#include "MIUtilMapIdToVariant.h"

//++ ------------------------------------------------------------------------------------
// Details:	CMIUtilMapIdToVariant constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMIUtilMapIdToVariant::CMIUtilMapIdToVariant( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	CMIUtilMapIdToVariant destructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMIUtilMapIdToVariant::~CMIUtilMapIdToVariant( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	Remove at the data from *this container.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
void CMIUtilMapIdToVariant::Clear( void )
{
	m_mapKeyToVariantValue.clear();
}

//++ ------------------------------------------------------------------------------------
// Details:	Check an ID is present already in *this container.
// Type:	Method.
// Args:	vId	- (R) Unique ID i.e. GUID.
// Return:	True - registered.
//			False - not found.
// Throws:	None.
//--
bool CMIUtilMapIdToVariant::HaveAlready( const CMIUtilString & vId ) const
{
	const MapKeyToVariantValue_t::const_iterator it = m_mapKeyToVariantValue.find( vId );
	if( it != m_mapKeyToVariantValue.end() )
		return true;
	
	return false;
}

//++ ------------------------------------------------------------------------------------
// Details:	Determine if *this container is currently holding any data.
// Type:	Method.
// Args:	None.
// Return:	bool	- True - Yes empty, false - one or more data object present.
// Throws:	None.
//--
bool CMIUtilMapIdToVariant::IsEmpty( void ) const
{
	return m_mapKeyToVariantValue.empty();
}

//++ ------------------------------------------------------------------------------------
// Details:	Check the ID is valid to be registered.
// Type:	Method.
// Args:	vId	- (R) Unique ID i.e. GUID.
// Return:	True - valid.
//			False - not valid.
// Throws:	None.
//--
bool CMIUtilMapIdToVariant::IsValid( const CMIUtilString & vId ) const
{
	bool bValid = true;

	if( vId.empty() )
		bValid = false;
	
	return bValid;
}

//++ ------------------------------------------------------------------------------------
// Details:	Remove from *this contain a data object specified by ID. The data object
//			when removed also calls its destructor should it have one.
// Type:	Method.
// Args:	vId	- (R) Unique ID i.e. GUID.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMIUtilMapIdToVariant::Remove( const CMIUtilString & vId )
{
	const MapKeyToVariantValue_t::const_iterator it = m_mapKeyToVariantValue.find( vId );
	if( it != m_mapKeyToVariantValue.end() )
	{
		m_mapKeyToVariantValue.erase( it );
	}

	return MIstatus::success;
}
