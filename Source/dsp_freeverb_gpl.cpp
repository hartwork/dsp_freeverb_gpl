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
#define PLUGIN_VERSION "0.333"


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
const char * const szCustom                 = "<Custom>";
const char * const szClassName              = "GPL_FREEVERB_HEADER_CLASS";
const char * const szIniSection             = "dsp_freeverb_gpl";
char * szWinampIni;

// Public settings
int iPreset = -1;
bool bActive;
float default_preset[] = { 0.50f, 0.25f, 0.33f, 0.75f, 1.00f, 0.80f };
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
HWND hConfig = NULL;
HWND hHeader;
WNDCLASS header_wc;
int header_width;
int header_height;
HWND hActive;
HWND hPresets;
HWND hSliders[ 6 ];
HWND hEdits[ 6 ];

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

float( revmodel:: * getter[ 5 ] )( void ) = {
	&revmodel::getroomsize,
	&revmodel::getdamp,
	&revmodel::getwet,
	&revmodel::getdry,
	&revmodel::getwidth
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



float * GetPresetData( int i )
{
	map <int, full_preset *>::iterator iter = preset_map.find( i );
	if( iter == preset_map.end() ) return NULL;
	return iter->second->data;
}



void ApplyPresetToReverb( float * data )
{
	if( !data ) return;

	for( int i = 0; i < 5; i++ )
	{
		( rev.*setter[ i ] )( fValues[ i ] = data[ i ] );
	}
	fVolume = data[ 5 ];
}



int init_freeverb( struct winampDSPModule * this_mod )
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
	header_wc.lpszClassName  = szClassName;
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
		bActive = ( 1 == GetPrivateProfileInt( szIniSection, "Active", 1, szWinampIni ) );

		// Read presets from ini
		const int iPresetCount = GetPrivateProfileInt( szIniSection, "PresetCount", 0, szWinampIni );
		char szKey[ 20 ];
		char szPresetData[ 200 ];
		for( int i = 0; i < iPresetCount; i++ )
		{
			sprintf( szKey, FORMAT_PRESET_TITLE_KEY, i );
			char * szPresetTitle = new char[ 200 ];
			const int iCharsTitle = GetPrivateProfileString( szIniSection, szKey, "", szPresetTitle, 200, szWinampIni );
			if( !iCharsTitle )
			{
				delete [] szPresetTitle;
				continue;
			}

			sprintf( szKey, FORMAT_PRESET_DATA_KEY, i );
			const int iCharsData = GetPrivateProfileString( szIniSection, szKey, "", szPresetData, 200, szWinampIni );
			if( !iCharsData )
			{
				delete [] szPresetTitle;
				continue;
			}
			
			// Try parsing the preset
			full_preset * preset = new full_preset;
			preset->szTitle = szPresetTitle;
			
			const int iFields = sscanf( szPresetData, FORMAT_PRESET_DATA_READ,
				preset->data, preset->data + 1, preset->data + 2,
				preset->data + 3, preset->data + 4, preset->data + 5 );
			
			// Add preset to map
			if( iFields == 6 )
			{
				preset_map.insert( pair<int, full_preset *>( iNextPresetId++, preset ) );
			}
			else
			{
				delete [] szPresetTitle;
				delete [] preset;
			}
		}
		
		const int iActivePreset = GetPrivateProfileInt( szIniSection, "ActivePreset", -1, szWinampIni );
		
		// <Custom>?
		if( iActivePreset == -1 )
		{
			char szPresetRecord[ 200 ];
			const int iChars = GetPrivateProfileString( szIniSection, "Custom_Data", "", szPresetRecord, 200, szWinampIni );
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
					iPreset = -1;
					ApplyPresetToReverb( preset_data );
					bApplyDefault = false;
				}
			}
			
			if( bApplyDefault )
			{
				iPreset = -1;
				ApplyPresetToReverb( default_preset );
			}
		}
		else
		{
			// Valid preset?
			float * data = GetPresetData( iActivePreset );
			if( data )
			{
				// Choose this one
				iPreset = iActivePreset;
				ApplyPresetToReverb( data );
			}
			else
			{
				// Set <Custom>
				iPreset = -1;
				ApplyPresetToReverb( GetPresetData( iPreset ) );
			}
		}
	}
	else
	{
		iPreset = -1;
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
	


void quit_freeverb( struct winampDSPModule * this_mod )
{
	// Close dialog if still hope
	// This can happen when switching the effect
	// plugin while the config is open
	if( hConfig )
	{
		EndDialog( hConfig, FALSE );
		hConfig = NULL;
	}


	// Cleanup
	DeleteObject( pen_gray );
	DeleteObject( pen_red );

	UnregisterClass( szClassName, mod_freeverb.hDllInstance );

	DeleteObject( hFont );

	if( backup_samples ) delete [] backup_samples;


	// Write config
	if( szWinampIni == NULL ) return;
	WritePrivateProfileInt( szIniSection, "Active", bActive ? 1 : 0, szWinampIni );
	WritePrivateProfileInt( szIniSection, "ActivePreset", iPreset, szWinampIni );
	WritePrivateProfileInt( szIniSection, "PresetCount", ( int )preset_map.size(), szWinampIni );

	// Save <Custom> preset
	char szPresetRecord[ 200 ];
	sprintf( szPresetRecord, FORMAT_PRESET_DATA_WRITE, fValues[ 0 ],
		fValues[ 1 ], fValues[ 2 ], fValues[ 3 ], fValues[ 4 ], fValues[ 5 ] );
	WritePrivateProfileString( szIniSection, "Custom_Data", szPresetRecord, szWinampIni );

	// Save and free user presets
	map <int, full_preset *>::iterator iter = preset_map.begin();
	int i = 0;
	char szKey[ 20 ];
	while( iter != preset_map.end() )
	{
		sprintf( szKey, FORMAT_PRESET_TITLE_KEY, i );
		WritePrivateProfileString( szIniSection, szKey, iter->second->szTitle, szWinampIni );

		sprintf( szKey, FORMAT_PRESET_DATA_KEY, i );
		float * const & data = iter->second->data;
		sprintf( szPresetRecord, FORMAT_PRESET_DATA_WRITE, data[ 0 ], data[ 1 ],
			data[ 2 ], data[ 3 ], data[ 4 ], data[ 5 ] );
		WritePrivateProfileString( szIniSection, szKey, szPresetRecord, szWinampIni );
		
		delete [] iter->second->szTitle;
		delete [] iter->second;
		
		i++;
		iter++;
	}
	
	preset_map.clear();
}



int apply_freeverb( struct winampDSPModule * this_mod, short int * samples, int numsamples, int bps, int nch, int srate )
{
	// Backup samples
	if( hConfig )
	{
		const int iBytesToCopy = numsamples * nch * bps / 8;
		if( iBufferBytes < iBytesToCopy )
		{
			if( iBufferBytes != 0 ) delete [] backup_samples;
			backup_samples = new short int[ iBytesToCopy ];
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
	if( hConfig )
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
		HDC hdc = GetDC( hHeader );
			StretchBlt( hdc, 0, 0, header_width, header_height, memDC, 0, 0, 576, 256, SRCCOPY );
		ReleaseDC( hHeader, hdc );
	}

	return numsamples;
}



// Applies slider position to the edit control content
// and the current reverb setting
void ApplySlider( HWND hSlider, int iPos )
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



void ApplyPresetToSliders( float * data )
{
	if( !data ) return;

	char szVal[ 5 ];
	for( int i = 0; i < 6; i++ )
	{
		float & fVal = data[ i ];
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



BOOL CALLBACK WndprocConfig( HWND hwnd, UINT message, WPARAM wp, LPARAM lp )
{
	switch( message )
	{
	case WM_INITDIALOG:
		{
			hConfig = hwnd;

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

			// Init power button
			CheckDlgButton( hwnd, IDC_ACTIVE, ( bActive ? BST_CHECKED : BST_UNCHECKED ) );

			// Create header
			RECT r;
			GetClientRect( hwnd, &r );
			header_width   = r.right - r.left;
			header_height  = 68;

			hHeader = CreateWindowEx(
				WS_EX_STATICEDGE,
				szClassName,
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
			hPresets = GetDlgItem( hwnd, IDC_PRESETS );
			LRESULT iIndex = SendMessage( hPresets, CB_ADDSTRING, 0, ( LPARAM )szCustom );
			SendMessage( hPresets, CB_SETITEMDATA, iIndex, ( LPARAM )-1 );

			// Add presets from map to combo box
			bool bPresetFound = false;
			map <int, full_preset *>::iterator iter = preset_map.begin();
			while( iter != preset_map.end() )
			{
				char * & szTitle = iter->second->szTitle;
				iIndex = SendMessage( hPresets, CB_ADDSTRING, 0, ( LPARAM )szTitle );
				SendMessage( hPresets, CB_SETITEMDATA, iIndex, ( LPARAM )iter->first );
				
				// Active preset?
				if( iter->first == iPreset )
				{
					SendMessage( hPresets, CB_SELECTSTRING, ( WPARAM )iIndex, ( LPARAM )szTitle );
					bPresetFound = true;
				}
				
				iter++;
			}

			// Select <Custom>
			if( !bPresetFound )
			{
				SendMessage( hPresets, CB_SELECTSTRING, ( WPARAM )iIndex, ( LPARAM )szCustom );
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

			// Center window on parent
			RECT rp;
			GetWindowRect( GetForegroundWindow(), &rp );
			const int iParentWidth   = rp.right - rp.left;
			const int iParentHeight  = rp.bottom - rp.top;

			RECT rf;
			GetWindowRect( hwnd, &rf );
			const int iFreeverbWidth   = rf.right - rf.left;
			const int iFreeverbHeight  = rf.bottom - rf.top;

			int ox = ( iParentWidth - iFreeverbWidth ) / 2 + rp.left;
			int oy = ( iParentHeight - iFreeverbHeight ) / 2 + rp.top;

			MoveWindow( hwnd, ox, oy, iFreeverbWidth, iFreeverbHeight, false );
		}
		return TRUE;

	case WM_SYSCOMMAND:
		switch( wp )
		{
			case SC_CLOSE:
			{
				EndDialog( hwnd, FALSE );
				return TRUE;
			}
		}
		break;

	case WM_COMMAND:
		{
			switch( LOWORD( wp ) )
			{
			case IDC_ACTIVE:
				bActive = ( IsDlgButtonChecked( hwnd, IDC_ACTIVE ) == BST_CHECKED );

			case IDC_PRESETS:
				if( HIWORD( wp ) == CBN_SELCHANGE )
				{
					const LRESULT iIndex = SendMessage( ( HWND )lp, CB_GETCURSEL, 0, 0 );
					const int iPresetIdent = ( int )SendMessage( ( HWND )lp, CB_GETITEMDATA, iIndex, 0 );
					iPreset = iPresetIdent;
					if( iPresetIdent == -1 ) break;
					float * data = GetPresetData( iPresetIdent );
					ApplyPresetToReverb( data );
					ApplyPresetToSliders( data );
				}
				break;
			
			case IDC_ADD:
				{
					// Existing preset selected?
					const LRESULT iIndex = SendMessage( hPresets, CB_GETCURSEL, 0, 0 );
					if( iIndex == CB_ERR )
					{
						// Does a preset with the current name exist?
						const int iLen = GetWindowTextLength( hPresets );
						if( !iLen ) break;
						char * szPresetTitle = new char[ iLen + 1 ];
						GetWindowText( hPresets, szPresetTitle, iLen + 1 );

						const LRESULT iIndex = SendMessage( hPresets, CB_FINDSTRINGEXACT, ( WPARAM )-1, ( LPARAM )szPresetTitle );
						if( iIndex == CB_ERR )
						{
							// Add new preset
							
							// Deny "<Custom>" prefix
							if( !strncmp( szCustom, szPresetTitle, strlen( szCustom ) ) )
							{
								delete [] szPresetTitle;
								break;
							}

							// Add combo box entry
							const LRESULT iIndex = SendMessage( hPresets, CB_ADDSTRING, 0, ( LPARAM )szPresetTitle );
							SendMessage( hPresets, CB_SETITEMDATA, iIndex, ( LPARAM )iNextPresetId );

							// Add preset to map
							full_preset * preset = new full_preset;
							preset->szTitle = szPresetTitle;
							memcpy( preset->data, fValues, 6 * sizeof( float ) );
							preset_map.insert( pair<int, full_preset *>( iNextPresetId++, preset ) );
						}
						else
						{
							delete [] szPresetTitle;

							// Overwrite old preset
							const int iPresetIdent = ( int )SendMessage( hPresets, CB_GETITEMDATA, iIndex, 0 );
							
							// Deny overwriting <Custom>
							if( iPresetIdent == -1 ) break;
							
							// Overwrite map entry
							map<int, full_preset *>::iterator iter = preset_map.find( iPresetIdent );
							memcpy( iter->second->data, fValues, 6 * sizeof( float ) );
						}
					}
					else
					{
						// Overwrite old preset
						const int iPresetIdent = ( int )SendMessage( hPresets, CB_GETITEMDATA, iIndex, 0 );
						
						// Deny overwriting <Custom>
						if( iPresetIdent == -1 ) break;
						
						// Overwrite map entry
						map<int, full_preset *>::iterator iter = preset_map.find( iPresetIdent );
						memcpy( iter->second->data, fValues, 6 * sizeof( float ) );
					}
					
					// Show the list so the user know his preset was saved
					SendMessage( hPresets, CB_SHOWDROPDOWN, TRUE, 0 );
					SetFocus( hPresets );
					return 0;
				}
				break;
				
			case IDC_SUB:
				{
					// Existing preset selected?
					const LRESULT iIndex = SendMessage( hPresets, CB_GETCURSEL, 0, 0 );
					if( iIndex == CB_ERR ) break;
					
					// <Custom> selected?
					const int iPresetIdent = ( int )SendMessage( hPresets, CB_GETITEMDATA, iIndex, 0 );
					if( iPresetIdent == -1 ) break;
					
					// Delete Preset
					SendMessage( hPresets, CB_DELETESTRING, iIndex, 0 );
					map<int, full_preset *>::iterator iter = preset_map.find( iPresetIdent );
					preset_map.erase( iter, iter );
					
					// Select <Custum>
					SendMessage( hPresets, CB_SELECTSTRING, ( WPARAM )-1, ( LPARAM )szCustom );
				}
				break;
				
			}
			break;

		}
	
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
			if( iPreset != -1 )
			{
				iPreset = -1;
				SendMessage( hPresets, CB_SELECTSTRING, ( WPARAM )-1, ( LPARAM )szCustom );
			}
		}
		break;

	}
	return 0;
}



void config_freeverb( struct winampDSPModule * this_mod )
{
	DialogBox( mod_freeverb.hDllInstance, MAKEINTRESOURCE( IDD_CONFIG ), NULL, WndprocConfig );
	hConfig = NULL;
}



winampDSPModule * getModule( int which )
{
	return ( which ? NULL : &mod_freeverb );
}



DLLEXPORT winampDSPHeader * winampDSPGetHeader2()
{
	return &header;
}
