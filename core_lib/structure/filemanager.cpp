/*

Pencil - Traditional Animation Software
Copyright (C) 2005-2007 Patrick Corrieri & Pascal Naidon
Copyright (C) 2013-2014 Matt Chiawen Chang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation;

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/


#include "filemanager.h"
#include "pencildef.h"
#include "JlCompress.h"
#include "fileformat.h"
#include "object.h"
#include "editorstate.h"



FileManager::FileManager( QObject *parent ) : QObject( parent ),
    mLog( "SaveLoader" )
{
    ENABLE_DEBUG_LOG( mLog, false );
}

Object* FileManager::load( QString strFileName )
{
    if ( !QFile::exists( strFileName ) )
    {
        qCDebug( mLog ) << "ERROR - File doesn't exist.";
        mError = Status::FILE_NOT_FOUND;
        return nullptr;
    }

    emit progressUpdated( 0.f );

    QString strMainXMLFile;  //< the location of main.xml
    QString strDataFolder;   //< the folder which contains all bitmap & vector image & sound files.
    QString strWorkingDir;   //< the folder that pcxl will uncompress to.

    // Test file format: new zipped .pclx or old .pcl?
    bool oldFormat = isOldForamt( strFileName );

    if ( oldFormat )
    {
        qCDebug( mLog ) << "Recognized Old Pencil File Format (*.pcl) !";

        strMainXMLFile = strFileName;
        strDataFolder  = strMainXMLFile + "." + PFF_OLD_DATA_DIR;
        strWorkingDir  = strDataFolder;
    }
    else
    {
        qCDebug( mLog ) << "Recognized New zipped Pencil File Format (*.pclx) !";

        strWorkingDir  = unzip( strFileName );
        strMainXMLFile = QDir( strWorkingDir ).filePath( PFF_XML_FILE_NAME );
        strDataFolder  = QDir( strWorkingDir ).filePath( PFF_DATA_DIR );

        qCDebug( mLog ) << "Working Folder=" << strWorkingDir;
    }
    qCDebug( mLog ) << "XML=" << strMainXMLFile;
    qCDebug( mLog ) << "Data Folder=" << strDataFolder;

    QScopedPointer<QFile> file( new QFile( strMainXMLFile ) );
    if ( !file->open( QFile::ReadOnly ) )
    {
        cleanUpWorkingFolder();
        mError = Status::ERROR_FILE_CANNOT_OPEN;
        return nullptr;
    }

    qCDebug( mLog ) << "Checking main XML file...";
    QDomDocument xmlDoc;
    if ( !xmlDoc.setContent( file.data() ) )
    {
        cleanUpWorkingFolder();
        mError = Status::ERROR_INVALID_XML_FILE;
        return nullptr;
    }

    QDomDocumentType type = xmlDoc.doctype();
    if ( !( type.name() == "PencilDocument" || type.name() == "MyObject" ) )
    {
        cleanUpWorkingFolder();
        mError = Status::ERROR_INVALID_PENCIL_FILE;
        return nullptr;
    }

    QDomElement root = xmlDoc.documentElement();
    if ( root.isNull() )
    {
        cleanUpWorkingFolder();
        mError = Status::ERROR_INVALID_PENCIL_FILE;
        return nullptr;
    }

    // Create object.
    qCDebug( mLog ) << "Start to load object..";
    Object* object = new Object();
    
    loadPalette( object );

    bool ok = true;
    
    if ( root.tagName() == "document" )
    {
        ok = loadObject( object, root, strDataFolder );
    }
    else if ( root.tagName() == "object" || root.tagName() == "MyOject" )   // old Pencil format (<=0.4.3)
    {
        ok = loadObjectOldWay( object, root, strDataFolder );
    }

    object->setFilePath( strFileName );

    return object;
}

bool FileManager::loadObject( Object* object, const QDomElement& root, const QString& strDataFolder )
{
    bool isOK = true;
    for ( QDomNode node = root.firstChild(); !node.isNull(); node = node.nextSibling() )
    {
        QDomElement element = node.toElement(); // try to convert the node to an element.
        if ( element.isNull() )
        { 
            continue;
        }

        if ( element.tagName() == "object" )
        {
            qCDebug( mLog ) << "Load object";
            isOK = object->loadXML( element, strDataFolder );
        }
        else if ( element.tagName() == "editor" )
        {
            EditorState* editorData = loadEditorState( element );
            object->setEditorData( editorData );
        }
        else
        {
            Q_ASSERT( false );
        }
        //progress = std::min( progress + 10, 100 );
        //emit progressValueChanged( progress );
    }

    return isOK;
}

bool FileManager::loadObjectOldWay( Object* object, const QDomElement& root, const QString& strDataFolder )
{
    return object->loadXML( root, strDataFolder );
}

bool FileManager::isOldForamt( QString fileName )
{
    QStringList zippedFileList = JlCompress::getFileList( fileName );
    return ( zippedFileList.empty() );
}

bool FileManager::save( Object* object, QString strFileName )
{
    if ( object == nullptr ) { return false; }

    QFileInfo fileInfo( strFileName );
    if ( fileInfo.isDir() ) { return false; }

    bool isOldFile = strFileName.endsWith( PFF_OLD_EXTENSION );

    QString strTempWorkingFolder;
    QString strMainXMLFile;
    QString strDataFolder;
    if ( isOldFile )
    {
        qCDebug( mLog ) << "Save in Old Pencil File Format (*.pcl) !";
        strMainXMLFile = strFileName;
        strDataFolder = strMainXMLFile + "." + PFF_OLD_DATA_DIR;
    }
    else
    {
        qCDebug( mLog ) << "Save in New zipped Pencil File Format (*.pclx) !";
        strTempWorkingFolder = createWorkingFolder( strFileName );
        qCDebug( mLog ) << "Temp Folder=" << strTempWorkingFolder;
        strMainXMLFile = QDir( strTempWorkingFolder ).filePath( PFF_XML_FILE_NAME );
        strDataFolder = QDir( strTempWorkingFolder ).filePath( PFF_OLD_DATA_DIR );
    }

    QFileInfo dataInfo( strDataFolder );
    if ( !dataInfo.exists() )
    {
        QDir dir( strDataFolder ); // the directory where filePath is or will be saved
        dir.mkpath( strDataFolder ); // creates a directory with the same name +".data"
    }

    // save data
    int layerCount = object->getLayerCount();
    qCDebug( mLog ) << QString( "Total layers = %1" ).arg( layerCount );

    for ( int i = 0; i < layerCount; ++i )
    {
        Layer* layer = object->getLayer( i );
        qCDebug( mLog ) << QString( "Saving Layer %1" ).arg( i ).arg( layer->mName );

        //progressValue = (i * 100) / nLayers;
        //progress.setValue( progressValue );
        switch ( layer->type() )
        {
        case Layer::BITMAP:
        case Layer::VECTOR:
        case Layer::SOUND:
            layer->save( strDataFolder );
            break;
        case Layer::CAMERA:
            break;
        }
    }

    // save palette
    object->savePalette( strDataFolder );

    // -------- save main XML file -----------
    QScopedPointer<QFile> file( new QFile( strMainXMLFile ) );
    if ( !file->open( QFile::WriteOnly | QFile::Text ) )
    {
        //QMessageBox::warning(this, "Warning", "Cannot write file");
        return false;
    }

    QDomDocument xmlDoc( "PencilDocument" );
    QDomElement root = xmlDoc.createElement( "document" );
    xmlDoc.appendChild( root );

    // save editor information
    //QDomElement editorElement = createDomElement( xmlDoc );
    //root.appendChild( editorElement );
    qCDebug( mLog ) << "Save Editor Node.";

    // save object
    QDomElement objectElement = object->saveXML( xmlDoc );
    root.appendChild( objectElement );
    qCDebug( mLog ) << "Save Object Node.";

    const int IndentSize = 2;

    QTextStream out( file.data() );
    xmlDoc.save( out, IndentSize );

    if ( !isOldFile )
    {
        qCDebug( mLog ) << "Now compressing data to PFF - PCLX ...";

        bool ok = JlCompress::compressDir( strFileName, strTempWorkingFolder );
        if ( !ok )
        {
            return false;
        }
        //removePFFTmpDirectory( strTempWorkingFolder ); // --removing temporary files

        qCDebug( mLog ) << "Compressed. File saved.";
    }

    object->setFilePath( strFileName );
    object->setModified( false );

    return true;
}

EditorState* FileManager::loadEditorState( QDomElement docElem )
{
    EditorState* data = new EditorState;
    if ( docElem.isNull() )
    {
        return data;
    }

    QDomNode tag = docElem.firstChild();

    while ( !tag.isNull() )
    {
        QDomElement element = tag.toElement(); // try to convert the node to an element.
        if ( element.isNull() )
        {
            continue;
        }

     
        
        tag = tag.nextSibling();
    }
    return data;
}


void FileManager::extractEditorStateData( const QDomElement& element, EditorState* data )
{
    Q_ASSERT( data );

    QString strName = element.tagName();
    if ( strName == "currentFrame" )
    {
        data->mCurrentFrame = element.attribute( "value" ).toInt();
    }
    else  if ( strName == "currentColor" )
    {
        int r = element.attribute( "r", "255" ).toInt();
        int g = element.attribute( "g", "255" ).toInt();
        int b = element.attribute( "b", "255" ).toInt();
        int a = element.attribute( "a", "255" ).toInt();

        data->mCurrentColor = QColor( r, g, b, a );;
    }
    else if ( strName == "currentLayer" )
    {
        data->mCurrentLayer =  element.attribute( "value", "0" ).toInt();
    }
    else if ( strName == "currentView" )
    {
        double m11 = element.attribute( "m11", "1" ).toDouble();
        double m12 = element.attribute( "m12", "0" ).toDouble();
        double m21 = element.attribute( "m21", "0" ).toDouble();
        double m22 = element.attribute( "m22", "1" ).toDouble();
        double dx = element.attribute( "dx", "0" ).toDouble();
        double dy = element.attribute( "dy", "0" ).toDouble();
        
        data->mCurrentView = QTransform( m11, m12, m21, m22, dx, dy );
    }
    else if ( strName == "fps" )
    {
        data->mFps = element.attribute( "value", "12" ).toInt();
    }
    else if ( strName == "isLoop" )
    {
        data->mIsLoop = ( element.attribute( "value", "false" ) == "true" );
    }
    else if ( strName == "isRangedPlayback" )
    {
        data->mIsRangedPlayback = ( element.attribute( "value", "false" ) == "true" );
    }
    else if ( strName == "markInFrame" )
    {
        data->mMarkInFrame = element.attribute( "value", "0" ).toInt();
    }
    else if ( strName == "markOutFrame" )
    {
        data->mMarkInFrame = element.attribute( "value", "15" ).toInt();
    }
}

void FileManager::cleanUpWorkingFolder()
{
    removePFFTmpDirectory( mstrLastTempFolder );
}

bool FileManager::loadPalette( Object* obj )
{
    qCDebug( mLog ) << "Load Palette..";

    if ( !obj->loadPalette( strDataFolder ) )
    {
        obj->loadDefaultPalette();
    }
    return true;
}

QString FileManager::createWorkingFolder( QString strFileName )
{
    QFileInfo fileInfo( strFileName );
    QString strTempWorkingFolder = QDir::tempPath() +
                                   "/Pencil2D/" +
                                   fileInfo.completeBaseName() + 
                                   PFF_TMP_DECOMPRESS_EXT;

    QDir dir( QDir::tempPath() );
    dir.mkpath( strTempWorkingFolder );

    return strTempWorkingFolder;
}

QString FileManager::unzip( QString strZipFile )
{
    QString strTempWorkingPath = createWorkingFolder( strZipFile );

    // --removes an old decompression directory first  - better approach
    removePFFTmpDirectory( strTempWorkingPath );

    // --creates a new decompression directory
    JlCompress::extractDir( strZipFile, strTempWorkingPath );

    mstrLastTempFolder = strTempWorkingPath;
    return strTempWorkingPath;
}

QList<ColourRef> FileManager::loadPaletteFile( QString strFilename )
{
    QFileInfo fileInfo( strFilename );
    if ( !fileInfo.exists() )
    {
        return QList<ColourRef>();
    }

    // TODO: Load Palette.
    return QList<ColourRef>();
}