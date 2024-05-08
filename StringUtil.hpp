#pragma once

#include <cstring>
#include <cstdarg>

#include "SimpleVector.hpp"

// These came from:
//#include "minorGems/util/stringUtils.h"

[[maybe_unused]] inline static char *stringDuplicate( const char *inString ) {
    
    char *returnBuffer = new char[ strlen( inString ) + 1 ];

    strcpy( returnBuffer, inString );

    return returnBuffer;    
    }

[[maybe_unused]] inline static char **split( const char *inString, const char *inSeparator, 
              int *outNumParts ) {
    SimpleVector<char *> *parts = new SimpleVector<char *>();
    
    char *workingString = stringDuplicate( inString );
    char *workingStart = workingString;

    unsigned int separatorLength = strlen( inSeparator );

    char *foundSeparator = strstr( workingString, inSeparator );

    while( foundSeparator != NULL ) {
        // terminate at separator        
        foundSeparator[0] = '\0';
        parts->push_back( stringDuplicate( workingString ) );

        // skip separator
        workingString = &( foundSeparator[ separatorLength ] );
        foundSeparator = strstr( workingString, inSeparator );
        }

    // add the remaining part, even if it is the empty string
    parts->push_back( stringDuplicate( workingString ) );

                      
    delete [] workingStart;

    *outNumParts = parts->size();
    char **returnArray = parts->getElementArray();
    
    delete parts;

    return returnArray;
    }

// visual studio doesn't have va_copy
// suggested fix here:
// https://stackoverflow.com/questions/558223/va-copy-porting-to-visual-c
#ifndef va_copy
    #define va_copy( dest, src ) ( dest = src )
#endif

[[maybe_unused]] inline static char *vautoSprintf( const char* inFormatString, va_list inArgList ) {
    
    va_list argListCopyA;
    
    va_copy( argListCopyA, inArgList );
    

    unsigned int bufferSize = 50;


    char *buffer = new char[ bufferSize ];
    
    int stringLength =
        vsnprintf( buffer, bufferSize, inFormatString, inArgList );


    if( stringLength != -1 ) {
        // follows C99 standard...
        // stringLength is the length of the string that would have been
        // written if the buffer was big enough

        //  not room for string and terminating \0 in bufferSize bytes
        if( (unsigned int)stringLength >= bufferSize ) {

            // need to reprint with a bigger buffer
            delete [] buffer;

            bufferSize = (unsigned int)( stringLength + 1 );

            buffer = new char[ bufferSize ];

            // can simply use vsprintf now
            vsprintf( buffer, inFormatString, argListCopyA );
            
            va_end( argListCopyA );
            return buffer;
            }
        else {
            // buffer was big enough

            // trim the buffer to fit the string
            char *returnString = stringDuplicate( buffer );
            delete [] buffer;
            
            va_end( argListCopyA );
            return returnString;
            }
        }
    else {
        // follows old ANSI standard
        // -1 means the buffer was too small

        // Note that some buggy non-C99 vsnprintf implementations
        // (notably MinGW)
        // do not return -1 if stringLength equals bufferSize (in other words,
        // if there is not enough room for the trailing \0).

        // Thus, we need to check for both
        //   (A)  stringLength == -1
        //   (B)  stringLength == bufferSize
        // below.
        
        // keep doubling buffer size until it's big enough
        while( stringLength == -1 || 
               (unsigned int)stringLength == bufferSize ) {

            delete [] buffer;

            if( (unsigned int)stringLength == bufferSize ) {
                // only occurs if vsnprintf implementation is buggy

                // might as well use the information, though
                // (instead of doubling the buffer size again)
                bufferSize = bufferSize + 1;
                }
            else {
                // double buffer size again
                bufferSize = 2 * bufferSize;
                }

            buffer = new char[ bufferSize ];
    
            va_list argListCopyB;
            va_copy( argListCopyB, argListCopyA );
            
            stringLength =
                vsnprintf( buffer, bufferSize, inFormatString, argListCopyB );

            va_end( argListCopyB );
            }

        // trim the buffer to fit the string
        char *returnString = stringDuplicate( buffer );
        delete [] buffer;

        va_end( argListCopyA );
        return returnString;
        }
    }

[[maybe_unused]] inline static char *autoSprintf( const char* inFormatString, ... ) {
    va_list argList;
    va_start( argList, inFormatString );
    
    char *result = vautoSprintf( inFormatString, argList );
    
    va_end( argList );
    
    return result;
    }

