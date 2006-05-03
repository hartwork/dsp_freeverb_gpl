////////////////////////////////////////////////////////////////////////////////
// GPL Freeverb Winamp Plugin
// 
// Copyright © 2006  Sebastian Pipping <webmaster@hartwork.org>
// 
// -->  http://www.hartwork.org
// 
// This source code is released under the GNU General Public License (GPL).
// See GPL.txt for details. Any non-GPL usage is strictly forbidden.
////////////////////////////////////////////////////////////////////////////////


#include "Winamp/Dsp.h"
#include "Winamp/wa_ipc.h"
#include "Components/revmodel.hpp"
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "resource.h"

#include <map>
using namespace std;



#ifdef __cplusplus
# define DLLEXPORT extern "C" __declspec( dllexport )
#else
# define DLLEXPORT __declspec( dllexport )
#endif

#define PLUGIN_TITLE   "GPL Freeverb"
#define PLUGIN_VERSION "0.7"


struct struct_full_preset
{
	char * szTitle;
	float data[ 6 ];
};
typedef struct struct_full_preset full_preset;



// Internal settings
const char * const FORMAT_PRESET_DATA_READ  = "%f,%f,%f,%f,%f,%f";
const char * const FORMAT_PRESET_DATA_WRITE = "%1.2f,%1.2f,%1.2f,%1.2f,%1.2f,%1.2f";
const char * const FORMAT_PRESET_DATA_KEY   = "Preset_%i_Data";
const char * const FORMAT_PRESET_TITLE_KEY  = "Preset_%i_Title";
const char * const KEY_CUSTOM_DATA          = "Custom_Data";
const char * const KEY_ACTIVE_PRESET        = "Active_Preset";
const char * const KEY_PRESET_COUNT         = "Preset_Count";
const char * const KEY_POWER_SWITCH         = "Active";

// Note:  Choose a new section name whenever the config
//        format changes to prevent transition trouble
const char * const SECTION_FREEVERB         = "GPL Freeverb";

const char * const CUSTOM_PRESET_TITLE      = "<Custom>";
const char * const HEADER_CLASSNAME         = "GPL_FREEVERB_HEADER_CLASS";

char * szWinampIni;



// Built-in presets
const char * const Preset_Metallic     = "Metallic";
const float Preset_Metallic_Data[ 6 ]  = { 0.31f, 0.37f, 1.00f, 0.50f, 0.69f, 0.73f };

const char * const Preset_Diecast      = "Diecast Live";
const float Preset_Diecast_Data[ 6 ]   = { 0.57f, 0.37f, 0.35f, 1.00f, 0.83f, 0.57f };

const char * const Preset_Space        = "More Space";
const float Preset_Space_Data[ 6 ]     = { 0.57f, 0.37f, 0.33f, 0.50f, 0.73f, 0.86f };

const char * const Preset_Little       = "A Little Bit";
const float Preset_Little_Data[ 6 ]    = { 0.30f, 0.35f, 0.25f, 0.50f, 0.15f, 0.95f };

const char * const Preset_Nice         = "Sounds Nice";
const float Preset_Nice_Data[ 6 ]      = { 0.56f, 0.25f, 0.50f, 1.00f, 0.18f, 0.50f };

const int CUSTOM_PRESET_ID             = -1;
int DEFAULT_PRESET_ID                  = CUSTOM_PRESET_ID; // Updated later
const float * const default_preset     = Preset_Diecast_Data;



// Public settings
int iPreset;
int iPresetBefore = CUSTOM_PRESET_ID;
bool bActive;
float fValues[ 6 ];
float & fSize   = fValues[ 0 ];
float & fDamp   = fValues[ 1 ];
float & fWet    = fValues[ 2 ];
float & fDry    = fValues[ 3 ];
float & fWidth  = fValues[ 4 ];
float & fVolume = fValues[ 5 ];

// Preset database
map <int, full_preset *> preset_map;
int iNextPresetId = 0;

// Dialog related
HWND hConfigDialog = NULL;
HWND hHeader;
WNDCLASS header_wc;
int header_width;
int header_height;
HWND hActive;
HWND hChoosePreset;
HWND hSliders[ 6 ];
HWND hEdits[ 6 ];

HWND hAddDialog = NULL;
HWND hEditTitle;

// Drawing
HFONT hFont = NULL;
HPEN pen_gray = NULL;
HPEN pen_red = NULL;
HDC memDC = NULL;      // Memory device context
HBITMAP	memBM = NULL;  // Memory bitmap (for memDC)
HBITMAP	oldBM = NULL;  // Old bitmap (from memDC)

// Timing
DWORD last = 0;
const int ms_between_frames = 1000 / 40;

// Sample backup
int iBufferBytes = 0;
short int * backup_samples = NULL;


// Backend access
revmodel rev;

void( revmodel:: * setter[ 5 ] )( float ) = {
	&revmodel::setroomsize,
	&revmodel::setdamp,
	&revmodel::setwet,
	&revmodel::setdry,
	&revmodel::setwidth
};



LRESULT CALLBACK WndprocHeader( HWND hwnd, UINT message, WPARAM wp, LPARAM lp );
void config_freeverb( struct winampDSPModule * this_mod );
int init_freeverb( struct winampDSPModule * this_mod );
int apply_freeverb( struct winampDSPModule * this_mod, short int * samples, int numsamples, int bps, int nch, int srate );
void quit_freeverb( struct winampDSPModule * this_mod );
winampDSPModule * getModule( int which );



winampDSPHeader header =
{
	DSP_HDRVER,
	PLUGIN_TITLE " " PLUGIN_VERSION,
	getModule
};

winampDSPModule mod_freeverb =
{
	"Freeverb",
	NULL,             // hwndParent
	NULL,			  // hDllInstance
	config_freeverb,
	init_freeverb,
	apply_freeverb,
	quit_freeverb
};



// Remove [" ]+ from preset title (begin and end)
bool TrimTitle( char * const szTitle )
{
	if( !szTitle ) return false;
	char * beg = szTitle;
	char * end = szTitle + strlen( szTitle ) - 1;
	while( ( beg < end ) && ( (*beg == '"' ) || ( *beg == ' ' ) ) ) beg++;
	while( ( end > beg ) && ( (*end == '"' ) || ( *end == ' ' ) ) ) end--;
	if( beg == end ) return false;
	if( beg > szTitle )
	{
		memmove( szTitle, beg, sizeof( char ) * ( end - beg + 1 ) );
		*( end - ( beg - szTitle ) + 1 ) = '\0';
	}
	return true;
}



// Lookup preset at map
float * GetPresetData( const int i )
{
	const map<int, full_preset *>::iterator iter = preset_map.find( i );
	if( iter == preset_map.end() ) return NULL;
	return iter->second->data;
}



void ApplyPresetToReverb( const float * const data )
{
	if( !data ) return;

	memcpy( fValues, data, sizeof( float ) * 6 );
	for( int i = 0; i < 5; i++ )
	{
		( rev.*setter[ i ] )( data[ i ] );
	}
}



int init_freeverb( struct winampDSPModule * const this_mod )
{
	HWND & hMain = mod_freeverb.hwndParent;	

	// Create pens for drawing
	pen_gray  = CreatePen( PS_SOLID, 0, GetSysColor( COLOR_APPWORKSPACE ) );
	pen_red   = CreatePen( PS_SOLID, 0, RGB( 255, 0, 0 ) );

	// Init reverb
	rev.setmode( 0.0f );

	// Create header window class
	ZeroMemory( &header_wc, sizeof( WNDCLASS ) );
	header_wc.lpfnWndProc    = WndprocHeader;
	header_wc.hInstance      = mod_freeverb.hDllInstance;
	header_wc.hCursor        = LoadCursor( NULL, IDC_ARROW );
	header_wc.hbrBackground  = ( HBRUSH )GetStockObject( WHITE_BRUSH );
	header_wc.lpszClassName  = HEADER_CLASSNAME;
	if( !RegisterClass( &header_wc ) ) return 1;

	// Create header font
	hFont = CreateFont(
		-26,                          // int nHeight
		0,                            // int nWidth
		0,                            // int nEscapement
		0,                            // int nOrientation
		FW_REGULAR,                   // int fnWeight
		TRUE,                         // DWORD fdwItalic
		FALSE,                        // DWORD fdwUnderline
		FALSE,                        // DWORD fdwStrikeOut
		ANSI_CHARSET,                 // DWORD fdwCharSet
		OUT_TT_PRECIS,                // DWORD fdwOutputPrecision
		CLIP_DEFAULT_PRECIS,          // DWORD fdwClipPrecision
		ANTIALIASED_QUALITY,          // DWORD fdwQuality
		FF_DONTCARE | DEFAULT_PITCH,  // DWORD fdwPitchAndFamily
		"Palatino Linotype"           // LPCTSTR lpszFace
	);

	// Get Winamp.ini path
	szWinampIni = ( char * )SendMessage( hMain, WM_WA_IPC, 0, IPC_GETINIFILE );	

	// Read config
	if( szWinampIni )
	{
		bActive = ( 1 == GetPrivateProfileInt( SECTION_FREEVERB, KEY_POWER_SWITCH, 1, szWinampIni ) );

		// Read presets from ini
		const int iPresetCount = GetPrivateProfileInt( SECTION_FREEVERB, KEY_PRESET_COUNT, 0, szWinampIni );
		if( iPresetCount == 0 )
		{
			// Add built-in presets
			full_preset * preset;

			preset = new full_preset; // N-001
			preset->szTitle = new char[ strlen( Preset_Metallic ) + 1 ]; // N-002
			if( preset && preset->szTitle ) // C-001-002
			{
				strcpy( preset->szTitle, Preset_Metallic );
				memcpy( preset->data, Preset_Metallic_Data, sizeof( float ) * 6 );
				preset_map.insert( pair<int, full_preset *>( iNextPresetId++, preset ) );
			}

			preset = new full_preset; // N-003
			preset->szTitle = new char[ strlen( Preset_Diecast ) + 1 ]; // N-004
			if( preset && preset->szTitle ) // C-003-004
			{
				strcpy( preset->szTitle, Preset_Diecast );
				memcpy( preset->data, Preset_Diecast_Data, sizeof( float ) * 6 );
					DEFAULT_PRESET_ID = iNextPresetId++;
				preset_map.insert( pair<int, full_preset *>( DEFAULT_PRESET_ID, preset ) );
			}

			preset = new full_preset; // N-005
			preset->szTitle = new char[ strlen( Preset_Space ) + 1 ]; // N-006
			if( preset && preset->szTitle ) // C-005-006
			{
				strcpy( preset->szTitle, Preset_Space );
				memcpy( preset->data, Preset_Space_Data, sizeof( float ) * 6 );
				preset_map.insert( pair<int, full_preset *>( iNextPresetId++, preset ) );
			}

			preset = new full_preset; // N-007
			preset->szTitle = new char[ strlen( Preset_Little ) + 1 ]; // N-008
			if( preset && preset->szTitle ) // C-007-008
			{
				strcpy( preset->szTitle, Preset_Little );
				memcpy( preset->data, Preset_Little_Data, sizeof( float ) * 6 );
				preset_map.insert( pair<int, full_preset *>( iNextPresetId++, preset ) );
			}

			preset = new full_preset; // N-016
			preset->szTitle = new char[ strlen( Preset_Nice ) + 1 ]; // N-017
			if( preset && preset->szTitle ) // C-016-017
			{
				strcpy( preset->szTitle, Preset_Nice );
				memcpy( preset->data, Preset_Nice_Data, sizeof( float ) * 6 );
				preset_map.insert( pair<int, full_preset *>( iNextPresetId++, preset ) );
			}
		}
		else
		{
			// Load existing presets
			char szKey[ 20 ];
			char szPresetData[ 200 ];
			for( int i = 0; i < iPresetCount; i++ )
			{
				// Get preset title
				sprintf( szKey, FORMAT_PRESET_TITLE_KEY, i );
				char * szPresetTitle = new char[ 200 ]; // N-009
				if( !szPresetTitle ) continue; // C-009
				const int iCharsTitle = GetPrivateProfileString( SECTION_FREEVERB, szKey, "", szPresetTitle, 200, szWinampIni );
				if( !iCharsTitle )
				{
					delete [] szPresetTitle; // D-009
					continue;
				}

				if( !TrimTitle( szPresetTitle ) )
				{
					delete [] szPresetTitle; // D-009
					continue;
				}

				// Get preset data
				sprintf( szKey, FORMAT_PRESET_DATA_KEY, i );
				const int iCharsData = GetPrivateProfileString( SECTION_FREEVERB, szKey, "", szPresetData, 200, szWinampIni );
				if( !iCharsData )
				{
					delete [] szPresetTitle; // D-009
					continue;
				}
				
				// Parse preset data
				full_preset * preset = new full_preset; // N-010
				if( !preset ) // C-010
				{
					delete [] szPresetTitle; // D-009
					continue;
				}
				preset->szTitle = szPresetTitle;
				
				const int iFields = sscanf( szPresetData, FORMAT_PRESET_DATA_READ,
					preset->data, preset->data + 1, preset->data + 2,
					preset->data + 3, preset->data + 4, preset->data + 5 );
				
				if( iFields != 6 )
				{
					delete [] szPresetTitle; // D-009
					delete preset; // D-010
				}


				// Add preset to map
				preset_map.insert( pair<int, full_preset *>( iNextPresetId++, preset ) );
			}
		}
		
		// Apply active preset
		const int iActivePreset = GetPrivateProfileInt( SECTION_FREEVERB, KEY_ACTIVE_PRESET, -2, szWinampIni );
		if( iActivePreset == CUSTOM_PRESET_ID ) // <Custom> preset
		{
			// Get <Custom> data
			char szPresetRecord[ 200 ];
			const int iChars = GetPrivateProfileString( SECTION_FREEVERB, KEY_CUSTOM_DATA, "", szPresetRecord, 200, szWinampIni );

			// Parse <Custom> data
			bool bApplyDefault = true;
			if( iChars )
			{
				float preset_data[ 6 ];
				const int iFields = sscanf( szPresetRecord, FORMAT_PRESET_DATA_READ,
					preset_data, preset_data + 1, preset_data + 2, preset_data + 3,
					preset_data + 4, preset_data + 5 );
				
				// Valid preset?
				if( iFields == 6 )
				{
					iPreset = CUSTOM_PRESET_ID;
					ApplyPresetToReverb( preset_data );
					bApplyDefault = false;
				}
			}
			
			if( bApplyDefault )
			{
				iPreset = CUSTOM_PRESET_ID;
				ApplyPresetToReverb( default_preset );
			}
		}
		else if( iActivePreset == -2 )
		{
			// Very likely first time ever
			// Have we just added the built-in presets?
			if( iPresetCount == 0 )
			{
				iPreset        = DEFAULT_PRESET_ID;
				iPresetBefore  = DEFAULT_PRESET_ID;
			}
			else
			{
				iPreset        = CUSTOM_PRESET_ID;
			}

			ApplyPresetToReverb( default_preset );
		}
		else
		{
			// Valid preset?
			float * const data = GetPresetData( iActivePreset );
			if( data )
			{
				// Choose this one
				iPreset        = iActivePreset;
				iPresetBefore  = iActivePreset;
				ApplyPresetToReverb( data );
			}
			else
			{
				// Set <Custom>
				iPreset = CUSTOM_PRESET_ID;
				ApplyPresetToReverb( default_preset );
			}
		}
	}
	else
	{
		// Set <Custom>
		iPreset = CUSTOM_PRESET_ID;
		ApplyPresetToReverb( default_preset );
	}

	return 0;	// Fine
}


BOOL WritePrivateProfileInt( LPCTSTR lpAppName, LPCTSTR lpKeyName, int iValue, LPCTSTR lpFileName )
{
	TCHAR szBuffer[ 16 ];
	wsprintf( szBuffer, TEXT( "%i" ), iValue );
    return( WritePrivateProfileString( lpAppName, lpKeyName, szBuffer, lpFileName ) );
}
	


void quit_freeverb( struct winampDSPModule * const this_mod )
{
	// Close dialog if still hope
	// This can happen when switching the effect
	// plugin while the config is open
	if( hConfigDialog )
	{
		if( hAddDialog )
		{
			EndDialog( hAddDialog, FALSE );
			hAddDialog = NULL;
		}

		EndDialog( hConfigDialog, FALSE );
		hConfigDialog = NULL;
	}


	// Cleanup
	DeleteObject( pen_gray );
	DeleteObject( pen_red );

	UnregisterClass( HEADER_CLASSNAME, mod_freeverb.hDllInstance );

	DeleteObject( hFont );

	if( backup_samples ) delete [] backup_samples; // D-012


	// Write config
	if( szWinampIni == NULL ) return;
	WritePrivateProfileInt( SECTION_FREEVERB, KEY_POWER_SWITCH, bActive ? 1 : 0, szWinampIni );
	WritePrivateProfileInt( SECTION_FREEVERB, KEY_ACTIVE_PRESET, iPreset, szWinampIni );
	WritePrivateProfileInt( SECTION_FREEVERB, KEY_PRESET_COUNT, ( int )preset_map.size(), szWinampIni );

	// Save <Custom> preset
	char szPresetRecord[ 200 ];
	sprintf( szPresetRecord, FORMAT_PRESET_DATA_WRITE, fValues[ 0 ],
		fValues[ 1 ], fValues[ 2 ], fValues[ 3 ], fValues[ 4 ], fValues[ 5 ] );
	WritePrivateProfileString( SECTION_FREEVERB, KEY_CUSTOM_DATA, szPresetRecord, szWinampIni );

	// Save and free user presets
	map<int, full_preset *>::iterator iter = preset_map.begin();
	int i = 0;
	char szKey[ 20 ];
	while( iter != preset_map.end() )
	{
		// Save preset title
		sprintf( szKey, FORMAT_PRESET_TITLE_KEY, i );
		char * szFinalTitle = new char[ strlen( iter->second->szTitle ) + 2 + 1 ]; // N-011
		if( !szFinalTitle ) // C-011
		{
			delete [] iter->second->szTitle; // D-002-004-006-008-009-013-017
			delete iter->second; // D-001-003-005-007-010-014-016
			iter++;
		}
		sprintf( szFinalTitle, "\"%s\"", iter->second->szTitle );
		WritePrivateProfileString( SECTION_FREEVERB, szKey, szFinalTitle, szWinampIni );
		delete [] szFinalTitle; // D-011

		// Save preset data
		sprintf( szKey, FORMAT_PRESET_DATA_KEY, i );
		float * const & data = iter->second->data;
		sprintf( szPresetRecord, FORMAT_PRESET_DATA_WRITE, data[ 0 ], data[ 1 ],
			data[ 2 ], data[ 3 ], data[ 4 ], data[ 5 ] );
		WritePrivateProfileString( SECTION_FREEVERB, szKey, szPresetRecord, szWinampIni );
		
		delete [] iter->second->szTitle; // D-002-004-006-008-009-013-017
		delete iter->second; // D-001-003-005-007-010-014-016
		
		i++;
		iter++;
	}
	
	preset_map.clear();
}



int apply_freeverb( struct winampDSPModule * const this_mod, short int * const samples, const int numsamples, const int bps, const int nch, const int srate )
{
	// Backup samples
	if( hConfigDialog )
	{
		const int iBytesToCopy = numsamples * nch * bps / 8;
		if( iBufferBytes < iBytesToCopy )
		{
			if( iBufferBytes != 0 ) delete [] backup_samples; // D-012
			backup_samples = new short int[ iBytesToCopy ]; // N-012
			if( !backup_samples ) return numsamples; // C-012
			iBufferBytes = iBytesToCopy;
		}
		memcpy( backup_samples, samples, iBytesToCopy );
	}

	// Process samples
	if( bActive )
	{
		rev.processreplace(
			samples,
			samples + 1,
			numsamples,
			2,
			fVolume
		);
	}

	// Draw waveform
	if( hConfigDialog )
	{
		const DWORD now = GetTickCount();
		if( now - last < ms_between_frames )
		{
			last = now;
			return numsamples;
		}
		last = now;

		// Clear background
		RECT rect = { 0, 0, 576, 256 };
		FillRect( memDC, &rect, ( HBRUSH )GetStockObject( WHITE_BRUSH ) );
		
		int val;


		// Draw old waveform
		SelectObject( memDC, pen_gray );

		// First osc
		val = 0;
		for( int y = 0; y < nch; y++ )
		{
			val += backup_samples[ 0 * nch + y ];
		}
		val /= nch;
		MoveToEx( memDC, 0, 127 - ( val >> 8 ), NULL );

		// Other osc
		for( int x = 1; x < 576; x++ )
		{
			val = 0;
			for( int y = 0; y < nch; y++ )
			{
				val += backup_samples[ x * nch + y ];
			}
			val /= nch;
			LineTo( memDC, x, 127 - ( val >> 8 ) );
		}

		// Draw new waveform
		if( bActive )
		{
			SelectObject( memDC, pen_red );

			// First osc
			val = 0;
			for( int y = 0; y < nch; y++ )
			{
				val += samples[ 0 * nch + y ];
			}
			val /= nch;
			MoveToEx( memDC, 0, 127 - ( val >> 8 ), NULL );

			// Other osc
			for( int x = 1; x < 576; x++ )
			{
				val = 0;
				for( int y = 0; y < nch; y++ )
				{
					val += samples[ x * nch + y ];
				}
				val /= nch;

				LineTo( memDC, x, 127 - ( val >> 8 ) );
			}
		}

		// Copy doublebuffer to window
		const HDC hdc = GetDC( hHeader );
			StretchBlt( hdc, 0, 0, header_width, header_height, memDC, 0, 0, 576, 256, SRCCOPY );
		ReleaseDC( hHeader, hdc );
	}

	return numsamples;
}



// Applies slider position to the edit control content
// and the current reverb setting
void ApplySlider( const HWND hSlider, const int iPos )
{
	const float fVal = iPos / 100.0f;
	char szVal[ 5 ];
	sprintf( szVal, "%1.2f", fVal );
	
	// Apply new value
	if( hSlider == hSliders[ 5 ] )
	{
		// Volume
		SetWindowText( hEdits[ 5 ], szVal );
		fVolume = fVal;
	}
	else
	{
		// Size, Damp, Wet, Dry, Width
		for( int i = 0; i < 5; i++ )
		{
			if( hSlider == hSliders[ i ] )
			{
				SetWindowText( hEdits[ i ], szVal );
				( rev.*setter[ i ] )( fValues[ i ] = fVal );
				break;
			}
		}
	}
}



void ApplyPresetToSliders( const float * const data )
{
	if( !data ) return;

	char szVal[ 5 ];
	for( int i = 0; i < 6; i++ )
	{
		const float & fVal = data[ i ];
		sprintf( szVal, "%1.2f", fVal );
		SetWindowText( hEdits[ i ], szVal );
		SendMessage( hSliders[ i ], TBM_SETPOS, TRUE, ( int )( fVal * 100 ) );
	}
}



LRESULT CALLBACK WndprocHeader( HWND hwnd, UINT message, WPARAM wp, LPARAM lp )
{
	switch( message )
	{
	case WM_TIMER:
		{
			const DWORD now = GetTickCount();
			if( now - last > 250 )
			{
				RedrawWindow( hHeader, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW );
				RedrawWindow( hHeader, NULL, NULL, RDW_UPDATENOW );
			}
		}
		break;

	case WM_PAINT:
		{
			const HDC hdc = GetDC( hwnd );
			const HFONT hOldFont = ( HFONT )SelectObject( hdc, hFont );
				TextOut( hdc, 23, 14, "GPL Freeverb", 12 );
			SelectObject( hdc, hOldFont );
			ReleaseDC( hHeader, hdc );
		}
		break;

	}
	return DefWindowProc( hwnd, message, wp, lp );
}



void CenterOnParent( const HWND hwnd, const HWND hParent )
{
	// Center window on parent
	RECT rp;
	GetWindowRect( hParent, &rp );
	const int iParentWidth   = rp.right - rp.left;
	const int iParentHeight  = rp.bottom - rp.top;

	RECT rf;
	GetWindowRect( hwnd, &rf );
	const int iTargetWidth   = rf.right - rf.left;
	const int iTargetHeight  = rf.bottom - rf.top;

	const int ox = ( iParentWidth - iTargetWidth ) / 2 + rp.left;
	const int oy = ( iParentHeight - iTargetHeight ) / 2 + rp.top;

	MoveWindow( hwnd, ox, oy, iTargetWidth, iTargetHeight, false );
}


HWND hOkay;


BOOL CALLBACK WndprocAdd( HWND hwnd, UINT message, WPARAM wp, LPARAM lp )
{
	static bool bOkayEnabled;

	switch( message )
	{
	case WM_INITDIALOG:
		{
			hAddDialog = hwnd;
			bOkayEnabled = false; // Important! Every time!
			if( iPresetBefore == CUSTOM_PRESET_ID ) iPresetBefore = iPreset;

			// Get handles
			hEditTitle  = GetDlgItem( hwnd, IDC_EDIT_TITLE );
			hOkay       = GetDlgItem( hwnd, IDOK );

			// Add presets from map to combo box
			int iIndex;
			map<int, full_preset *>::iterator iter = preset_map.begin();
			while( iter != preset_map.end() )
			{
				char * & szTitle = iter->second->szTitle;
				iIndex = SendMessage( hEditTitle, CB_ADDSTRING, 0, ( LPARAM )szTitle );
				SendMessage( hEditTitle, CB_SETITEMDATA, iIndex, ( LPARAM )iter->first );
				
				// Is the the last preset before it became <Custom>?
				if( iter->first == iPresetBefore )
				{
					SendMessage( hEditTitle, CB_SETCURSEL, ( WPARAM )iIndex, 0 );
					EnableWindow( hOkay, TRUE );
					bOkayEnabled = true;
				}
				
				iter++;
			}

			CenterOnParent( hwnd, hConfigDialog );
		}
		return TRUE;

	case WM_COMMAND:
		{
			switch( LOWORD( wp ) )
			{
			case IDC_EDIT_TITLE:
				if( HIWORD( wp ) == CBN_EDITUPDATE )
				{
					// Enable/disable okay button				
					const int iLen = GetWindowTextLength( hEditTitle );
					const bool bShouldBeEnabled = ( iLen != 0 );
					if( bShouldBeEnabled != bOkayEnabled )
					{
						EnableWindow( hOkay, bShouldBeEnabled ? TRUE : FALSE );
						bOkayEnabled = bShouldBeEnabled;
					}
				}
				break;

			case IDCANCEL:
				// Escape/cancel has been pressed
				EndDialog( hwnd, FALSE );
				break;

			case IDOK:
				{
					// Existing preset selected?
					const LRESULT iIndex = SendMessage( hEditTitle, CB_GETCURSEL, 0, 0 );
					if( iIndex == CB_ERR ) // == No
					{
						// Get title
						const int iLen = GetWindowTextLength( hEditTitle );
						if( !iLen ) break;
						char * const szPresetTitle = new char[ iLen + 1 ]; // N-013
						if( !szPresetTitle ) break; // C-013
						GetWindowText( hEditTitle, szPresetTitle, iLen + 1 );

						// Trim title
						const bool bOkay = TrimTitle( szPresetTitle );
						if( !bOkay )
						{
							delete [] szPresetTitle; // D-013
							break;
						}

						// Deny "<Custom>" prefix
						if( !strncmp( CUSTOM_PRESET_TITLE, szPresetTitle, strlen( CUSTOM_PRESET_TITLE ) ) )
						{
							delete [] szPresetTitle; // D-013
							break;
						}

						// Does a preset with the current name exist?
						const LRESULT iIndex = SendMessage( hEditTitle, CB_FINDSTRINGEXACT, ( WPARAM )-1, ( LPARAM )szPresetTitle );
						if( iIndex == CB_ERR ) // == No
						{
							const int iPresetId = iNextPresetId++;

							// Add preset to map
							full_preset * preset = new full_preset; // N-014
							if( !preset ) // C-014
							{
								delete [] szPresetTitle; // D-013
								break;
							}
							preset->szTitle = szPresetTitle;
							memcpy( preset->data, fValues, 6 * sizeof( float ) );
							preset_map.insert( pair<int, full_preset *>( iPresetId, preset ) );

							// Add combo box entry
							const LRESULT iIndex = SendMessage( hChoosePreset, CB_ADDSTRING, 0, ( LPARAM )szPresetTitle );
							SendMessage( hChoosePreset, CB_SETITEMDATA, iIndex, ( LPARAM )iPresetId );

							// Select new preset
							iPreset        = iPresetId;
							iPresetBefore  = iPresetId;
							SendMessage( hChoosePreset, CB_SELECTSTRING, ( WPARAM )-1, ( LPARAM )szPresetTitle );
						}
						else
						{
							// Overwrite old preset
							const int iPresetId = ( int )SendMessage( hEditTitle, CB_GETITEMDATA, iIndex, 0 );
							
							// Overwrite map entry
							const map<int, full_preset *>::iterator iter = preset_map.find( iPresetId );
							memcpy( iter->second->data, fValues, 6 * sizeof( float ) );

							// Select old-new preset
							iPreset        = iPresetId;
							iPresetBefore  = iPresetId;
							SendMessage( hChoosePreset, CB_SELECTSTRING, ( WPARAM )-1, ( LPARAM )szPresetTitle );

							delete [] szPresetTitle; // D-013
						}
					}
					else // == Yes
					{
						// Overwrite old preset
						const int iPresetId = ( int )SendMessage( hEditTitle, CB_GETITEMDATA, iIndex, 0 );
						
						// Overwrite map entry
						const map<int, full_preset *>::iterator iter = preset_map.find( iPresetId );
						memcpy( iter->second->data, fValues, 6 * sizeof( float ) );

						// Get title
						const int iLen = GetWindowTextLength( hEditTitle );
						if( !iLen ) break;
						char * const szPresetTitle = new char[ iLen + 1 ]; // N-015
						if( !szPresetTitle ) break; // C-015
						GetWindowText( hEditTitle, szPresetTitle, iLen + 1 );

						// Select old-new preset
						iPreset        = iPresetId;
						iPresetBefore  = iPresetId;
						SendMessage( hChoosePreset, CB_SELECTSTRING, ( WPARAM )-1, ( LPARAM )szPresetTitle );

						delete [] szPresetTitle; // D-015
					}
					
					EndDialog( hwnd, FALSE );
				}
				break;

			}
		}
        break;

	}
	return 0;
}



BOOL CALLBACK WndprocConfig( HWND hwnd, UINT message, WPARAM wp, LPARAM lp )
{
	switch( message )
	{
	case WM_INITDIALOG:
		{
			hConfigDialog = hwnd;

			// Get handles
			hSliders[ 0 ] = GetDlgItem( hwnd, IDC_SLIDER_SIZE  );
			hSliders[ 1 ] = GetDlgItem( hwnd, IDC_SLIDER_DAMP  );
			hSliders[ 2 ] = GetDlgItem( hwnd, IDC_SLIDER_WET   );
			hSliders[ 3 ] = GetDlgItem( hwnd, IDC_SLIDER_DRY   );
			hSliders[ 4 ] = GetDlgItem( hwnd, IDC_SLIDER_WIDTH );
			hSliders[ 5 ] = GetDlgItem( hwnd, IDC_SLIDER_VOL   );

			hEdits[ 0 ] = GetDlgItem( hwnd, IDC_EDIT_SIZE  );
			hEdits[ 1 ] = GetDlgItem( hwnd, IDC_EDIT_DAMP  );
			hEdits[ 2 ] = GetDlgItem( hwnd, IDC_EDIT_WET   );
			hEdits[ 3 ] = GetDlgItem( hwnd, IDC_EDIT_DRY   );
			hEdits[ 4 ] = GetDlgItem( hwnd, IDC_EDIT_WIDTH );
			hEdits[ 5 ] = GetDlgItem( hwnd, IDC_EDIT_VOL   );

			hActive  = GetDlgItem( hwnd, IDC_ACTIVE );
			hChoosePreset = GetDlgItem( hwnd, IDC_CHOOSE_PRESET );

			// Init power button
			CheckDlgButton( hwnd, IDC_ACTIVE, ( bActive ? BST_CHECKED : BST_UNCHECKED ) );

			// Create header
			RECT r;
			GetClientRect( hwnd, &r );
			header_width   = r.right - r.left;
			header_height  = 68;

			hHeader = CreateWindowEx(
				WS_EX_STATICEDGE,
				HEADER_CLASSNAME,
				"",
				WS_CHILD | WS_VISIBLE,
				0,
				0,
				header_width,
				header_height,
				hwnd,
				NULL,
				mod_freeverb.hDllInstance,
				0
			);

			SetTimer( hHeader, 123, 250, NULL );

			// Create doublebuffer
			const HDC hdc = GetDC( hHeader );
				memDC = CreateCompatibleDC( hdc );
				memBM = CreateCompatibleBitmap( hdc, 576, 256 );
				oldBM = ( HBITMAP )SelectObject( memDC, memBM );
			ReleaseDC( hHeader, hdc );

			// Init combo box
			// Add <Custum> entry
			LRESULT iIndex = SendMessage( hChoosePreset, CB_ADDSTRING, 0, ( LPARAM )CUSTOM_PRESET_TITLE );
			SendMessage( hChoosePreset, CB_SETITEMDATA, iIndex, ( LPARAM )CUSTOM_PRESET_ID );

			// Add presets from map to combo box
			bool bPresetFound = false;
			map<int, full_preset *>::iterator iter = preset_map.begin();
			while( iter != preset_map.end() )
			{
				char * & szTitle = iter->second->szTitle;
				iIndex = SendMessage( hChoosePreset, CB_ADDSTRING, 0, ( LPARAM )szTitle );
				SendMessage( hChoosePreset, CB_SETITEMDATA, iIndex, ( LPARAM )iter->first );
				
				// Active preset?
				if( iter->first == iPreset )
				{
					SendMessage( hChoosePreset, CB_SELECTSTRING, ( WPARAM )iIndex, ( LPARAM )szTitle );
					bPresetFound = true;
				}
				
				iter++;
			}

			// Select <Custom>
			if( !bPresetFound )
			{
				SendMessage( hChoosePreset, CB_SELECTSTRING, ( WPARAM )iIndex, ( LPARAM )CUSTOM_PRESET_TITLE );
			}

			// Init sliders and edits
			int i;
			char szVal[ 5 ];
			for( i = 0; i < 6; i++ )
			{
				float & fVal = fValues[ i ];
				SendMessage( hSliders[ i ], TBM_SETRANGE, FALSE, MAKELONG( 0, 100 ) );
				SendMessage( hSliders[ i ], TBM_SETPOS, TRUE, ( int )( fVal * 100 ) );
				SendMessage( hSliders[ i ], TBM_SETTHUMBLENGTH, 13, 0 );
				sprintf( szVal, "%1.2f", fVal );
				SetWindowText( hEdits[ i ], szVal );
			}

			// Set window title
			char szTitle[ 512 ] = "";
			wsprintf( szTitle, "%s %s", PLUGIN_TITLE, PLUGIN_VERSION );
			SetWindowText( hwnd, szTitle );


			CenterOnParent( hwnd, GetForegroundWindow() );
		}
		return TRUE;

	case WM_SYSCOMMAND:
		switch( wp )
		{
		case SC_CLOSE:
			EndDialog( hwnd, FALSE );
			return TRUE;
		}
		break;

	case WM_COMMAND:
		{
			switch( LOWORD( wp ) )
			{
			case IDCANCEL:
				// Escape has been pressed
				EndDialog( hwnd, FALSE );
				break;

			case IDC_ACTIVE:
				bActive = ( IsDlgButtonChecked( hwnd, IDC_ACTIVE ) == BST_CHECKED );
				break;

			case IDC_CHOOSE_PRESET:
				if( HIWORD( wp ) == CBN_SELCHANGE )
				{
					const LRESULT iIndex = SendMessage( ( HWND )lp, CB_GETCURSEL, 0, 0 );
					const int iPresetId = ( int )SendMessage( ( HWND )lp, CB_GETITEMDATA, iIndex, 0 );
					iPreset        = iPresetId;
					iPresetBefore  = iPresetId;
					if( iPresetId == CUSTOM_PRESET_ID ) break;
					float * data = GetPresetData( iPresetId );
					ApplyPresetToReverb( data );
					ApplyPresetToSliders( data );
				}
				break;
			
			case IDC_ADD:
				DialogBox( mod_freeverb.hDllInstance, MAKEINTRESOURCE( IDD_ADD ), hwnd, WndprocAdd );
				hAddDialog = NULL;
				break;
				
			case IDC_SUB:
				{
					// Existing preset selected?
					const LRESULT iIndex = SendMessage( hChoosePreset, CB_GETCURSEL, 0, 0 );
					if( iIndex == CB_ERR ) break;
					
					// <Custom> selected?
					const int iPresetId = ( int )SendMessage( hChoosePreset, CB_GETITEMDATA, iIndex, 0 );
					if( iPresetId == CUSTOM_PRESET_ID ) break;
					
					// Delete Preset from combo box
					SendMessage( hChoosePreset, CB_DELETESTRING, iIndex, 0 );

					// Select <Custum>
					iPreset        = CUSTOM_PRESET_ID;
					iPresetBefore  = CUSTOM_PRESET_ID;
					SendMessage( hChoosePreset, CB_SELECTSTRING, ( WPARAM )-1, ( LPARAM )CUSTOM_PRESET_TITLE );

					// Delete preset from map
					const map<int, full_preset *>::iterator iter = preset_map.find( iPresetId );
					delete [] iter->second->szTitle; // D-002-004-006-008-009-013-017
					delete iter->second; // D-001-003-005-007-010-014-016
					preset_map.erase( iter );
				}
				break;

			}
		}
		break;
	
	case WM_HSCROLL:
		{
			const HWND hSlider = ( HWND )lp;
			if( !hSlider ) break;

			int iPos;
			switch( LOWORD( wp ) )
			{
			case SB_THUMBPOSITION:
			case SB_THUMBTRACK:
				iPos = HIWORD( wp );
				break;
				
			default:
				iPos = ( int )SendMessage( hSlider, TBM_GETPOS, 0, 0 );	

			}
			ApplySlider( hSlider, iPos );

			// Switch to <Custum>
			if( iPreset != CUSTOM_PRESET_ID )
			{
				iPresetBefore  = iPreset;
				iPreset        = CUSTOM_PRESET_ID;
				SendMessage( hChoosePreset, CB_SELECTSTRING, ( WPARAM )-1, ( LPARAM )CUSTOM_PRESET_TITLE );
			}
		}
		break;

	}
	return 0;
}



void config_freeverb( struct winampDSPModule * const this_mod )
{
	static bool bCheapLocked = false;
	if( bCheapLocked ) return;
	bCheapLocked = true;

		DialogBox( mod_freeverb.hDllInstance, MAKEINTRESOURCE( IDD_CONFIG ), NULL, WndprocConfig );
		hConfigDialog = NULL;

	bCheapLocked = false;
}



winampDSPModule * getModule( const int which )
{
	return ( which ? NULL : &mod_freeverb );
}



DLLEXPORT winampDSPHeader * winampDSPGetHeader2()
{
	return &header;
}
