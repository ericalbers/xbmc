/*
 *      Copyright (c) 2007 Frodo/jcmarshall/vulkanr/d4rk
 *      Based on XBoxRenderer by Frodo/jcmarshall
 *      Portions Copyright (c) by the authors of ffmpeg / xvid /mplayer
 *      Copyright (C) 2007-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
#include "system.h"

#include <locale.h>

#include "LinuxRendererGL.h"
#include "Application.h"
#include "RenderFactory.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "VideoShaders/YUV2RGBShaderGL.h"
#include "VideoShaders/VideoFilterShaderGL.h"
#include "windowing/WindowingFactory.h"
#include "guilib/Texture.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/MatrixGLES.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/GLUtils.h"
#include "utils/StringUtils.h"
#include "RenderCapture.h"
#include "cores/IPlayer.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecUtils.h"
#include "cores/FFmpeg.h"

#ifdef TARGET_DARWIN_OSX
#include "platform/darwin/osx/CocoaInterface.h"
#include <CoreVideo/CoreVideo.h>
#include <OpenGL/CGLIOSurface.h>
#include "platform/darwin/DarwinUtils.h"
#endif

//! @bug
//! due to a bug on osx nvidia, using gltexsubimage2d with a pbo bound and a null pointer
//! screws up the alpha, an offset fixes this, there might still be a problem if stride + PBO_OFFSET
//! is a multiple of 128 and deinterlacing is on
#define PBO_OFFSET 16

#define BUFFER_OFFSET(i) ((char *)NULL + (i))

using namespace Shaders;

static const GLubyte stipple_weave[] = {
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
};

CLinuxRendererGL::YUVBUFFER::YUVBUFFER()
{
  memset(&fields, 0, sizeof(fields));
  memset(&image , 0, sizeof(image));
  memset(&pbo   , 0, sizeof(pbo));
  videoBuffer = nullptr;
  loaded = false;
}

CLinuxRendererGL::YUVBUFFER::~YUVBUFFER() = default;

CBaseRenderer* CLinuxRendererGL::Create(CVideoBuffer *buffer)
{
  return new CLinuxRendererGL();
}

bool CLinuxRendererGL::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("default", CLinuxRendererGL::Create);
  return true;
}

CLinuxRendererGL::CLinuxRendererGL()
{
  m_textureTarget = GL_TEXTURE_2D;

  m_renderMethod = RENDER_GLSL;
  m_renderQuality = RQ_SINGLEPASS;
  m_iFlags = 0;
  m_format = AV_PIX_FMT_NONE;

  m_iYV12RenderBuffer = 0;
  m_currentField = FIELD_FULL;
  m_reloadShaders = 0;
  m_pYUVShader = nullptr;
  m_pVideoFilterShader = nullptr;
  m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
  m_scalingMethodGui = (ESCALINGMETHOD)-1;
  m_useDithering = CServiceBroker::GetSettings().GetBool("videoscreen.dither");
  m_ditherDepth = CServiceBroker::GetSettings().GetInt("videoscreen.ditherdepth");
  m_fullRange = !g_Windowing.UseLimitedColor();

  m_fbo.width = 0.0;
  m_fbo.height = 0.0;
  m_NumYV12Buffers = 0;
  m_bConfigured = false;
  m_bValidated = false;
  m_clearColour = 0.0f;
  m_pboSupported = false;
  m_pboUsed = false;
  m_nonLinStretch = false;
  m_nonLinStretchGui = false;
  m_pixelRatio = 0.0f;

  m_ColorManager.reset(new CColorManager());
  m_tCLUTTex = 0;
  m_CLUT = NULL;
  m_CLUTsize = 0;
  m_cmsToken = -1;
  m_cmsOn = false;
}

CLinuxRendererGL::~CLinuxRendererGL()
{
  UnInit();

  if (m_pYUVShader)
  {
    m_pYUVShader->Free();
    delete m_pYUVShader;
    m_pYUVShader = nullptr;
  }

  if (m_pVideoFilterShader)
  {
    delete m_pVideoFilterShader;
    m_pVideoFilterShader = nullptr;
  }
}

bool CLinuxRendererGL::ValidateRenderer()
{
  if (!m_bConfigured)
    return false;

  // if its first pass, just init textures and return
  if (ValidateRenderTarget())
    return false;

  int index = m_iYV12RenderBuffer;
  YUVBUFFER& buf = m_buffers[index];

  if (!buf.fields[FIELD_FULL][0].id)
    return false;

  return true;
}

bool CLinuxRendererGL::ValidateRenderTarget()
{
  if (!m_bValidated)
  {
    if (!g_Windowing.IsExtSupported("GL_ARB_texture_non_power_of_two") &&
         g_Windowing.IsExtSupported("GL_ARB_texture_rectangle"))
    {
      m_textureTarget = GL_TEXTURE_RECTANGLE_ARB;
    }

    // function pointer for texture might change in
    // call to LoadShaders
    glFinish();
    for (int i = 0 ; i < NUM_BUFFERS ; i++)
      DeleteTexture(i);

    // trigger update of video filters
    m_scalingMethodGui = (ESCALINGMETHOD)-1;

     // create the yuv textures
    UpdateVideoFilter();
    LoadShaders();
    if (m_renderMethod < 0)
      return false;

    if (m_textureTarget == GL_TEXTURE_RECTANGLE_ARB)
      CLog::Log(LOGNOTICE, "Using GL_TEXTURE_RECTANGLE_ARB");
    else
      CLog::Log(LOGNOTICE, "Using GL_TEXTURE_2D");

    for (int i = 0 ; i < m_NumYV12Buffers ; i++)
      CreateTexture(i);

    m_bValidated = true;
    return true;
  }
  return false;
}

bool CLinuxRendererGL::Configure(const VideoPicture &picture, float fps, unsigned flags, unsigned int orientation)
{
  m_format = picture.videoBuffer->GetFormat();
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;
  m_fps = fps;

  // Save the flags.
  m_iFlags = flags;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ViewMode);
  ManageRenderArea();

  m_bConfigured = true;
  m_scalingMethodGui = (ESCALINGMETHOD)-1;

  // Ensure that textures are recreated and rendering starts only after the 1st
  // frame is loaded after every call to Configure().
  m_bValidated = false;

  m_nonLinStretch    = false;
  m_nonLinStretchGui = false;
  m_pixelRatio       = 1.0;

  m_pboSupported = g_Windowing.IsExtSupported("GL_ARB_pixel_buffer_object");

  // setup the background colour
  m_clearColour = g_Windowing.UseLimitedColor() ? (16.0f / 0xff) : 0.0f;

#ifdef TARGET_DARWIN_OSX
  // on osx 10.9 mavericks we get a strange ripple
  // effect when rendering with pbo
  // when used on intel gpu - we have to quirk it here
  if (CDarwinUtils::IsMavericksOrHigher())
  {
    std::string rendervendor = g_Windowing.GetRenderVendor();
    StringUtils::ToLower(rendervendor);
    if (rendervendor.find("intel") != std::string::npos)
      m_pboSupported = false;
  }
#endif

  // load 3DLUT
  if (m_ColorManager->IsEnabled())
  {
    if (!m_ColorManager->CheckConfiguration(m_cmsToken, m_iFlags))
    {
      CLog::Log(LOGDEBUG, "CMS configuration changed, reload LUT");
      if (!LoadCLUT())
        return false;
    }
    m_cmsOn = true;
  }
  else
  {
    m_cmsOn = false;
  }

  return true;
}

bool CLinuxRendererGL::ConfigChanged(const VideoPicture &picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;

  return false;
}

void CLinuxRendererGL::AddVideoPicture(const VideoPicture &picture, int index, double currentClock)
{
  YUVBUFFER &buf = m_buffers[index];
  buf.videoBuffer = picture.videoBuffer;
  buf.videoBuffer->Acquire();
  buf.loaded = false;
}

void CLinuxRendererGL::ReleaseBuffer(int idx)
{
  YUVBUFFER &buf = m_buffers[idx];
  if (buf.videoBuffer)
  {
    buf.videoBuffer->Release();
    buf.videoBuffer = nullptr;
  }
}

void CLinuxRendererGL::GetPlaneTextureSize(YUVPLANE& plane)
{
  /* texture is assumed to be bound */
  GLint width  = 0
      , height = 0
      , border = 0;
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_WIDTH , &width);
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_HEIGHT, &height);
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_BORDER, &border);
  plane.texwidth  = width  - 2 * border;
  plane.texheight = height - 2 * border;
  if(plane.texwidth <= 0 || plane.texheight <= 0)
  {
    CLog::Log(LOGDEBUG, "CLinuxRendererGL::GetPlaneTextureSize - invalid size %dx%d - %d", width, height, border);
    /* to something that avoid division by zero */
    plane.texwidth  = 1;
    plane.texheight = 1;
  }
}

void CLinuxRendererGL::CalculateTextureSourceRects(int source, int num_planes)
{
  YUVBUFFER& buf = m_buffers[source];
  YuvImage* im  = &buf.image;

  // calculate the source rectangle
  for(int field = 0; field < 3; field++)
  {
    for(int plane = 0; plane < num_planes; plane++)
    {
      YUVPLANE& p = buf.fields[field][plane];

      p.rect = m_sourceRect;
      p.width  = im->width;
      p.height = im->height;

      if(field != FIELD_FULL)
      {
        /* correct for field offsets and chroma offsets */
        float offset_y = 0.5;
        if(plane != 0)
          offset_y += 0.5;
        if(field == FIELD_BOT)
          offset_y *= -1;

        p.rect.y1 += offset_y;
        p.rect.y2 += offset_y;

        /* half the height if this is a field */
        p.height  *= 0.5f;
        p.rect.y1 *= 0.5f;
        p.rect.y2 *= 0.5f;
      }

      if(plane != 0)
      {
        p.width   /= 1 << im->cshift_x;
        p.height  /= 1 << im->cshift_y;

        p.rect.x1 /= 1 << im->cshift_x;
        p.rect.x2 /= 1 << im->cshift_x;
        p.rect.y1 /= 1 << im->cshift_y;
        p.rect.y2 /= 1 << im->cshift_y;
      }

      // protect against division by zero
      if (p.texheight == 0 || p.texwidth == 0 ||
          p.pixpertex_x == 0 || p.pixpertex_y == 0)
      {
        continue;
      }

      p.height  /= p.pixpertex_y;
      p.rect.y1 /= p.pixpertex_y;
      p.rect.y2 /= p.pixpertex_y;
      p.width   /= p.pixpertex_x;
      p.rect.x1 /= p.pixpertex_x;
      p.rect.x2 /= p.pixpertex_x;

      if (m_textureTarget == GL_TEXTURE_2D)
      {
        p.height  /= p.texheight;
        p.rect.y1 /= p.texheight;
        p.rect.y2 /= p.texheight;
        p.width   /= p.texwidth;
        p.rect.x1 /= p.texwidth;
        p.rect.x2 /= p.texwidth;
      }
    }
  }
}

void CLinuxRendererGL::LoadPlane(YUVPLANE& plane, int type,
                                 unsigned width, unsigned height,
                                 int stride, int bpp, void* data)
{

  if (plane.pbo)
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, plane.pbo);

  int bps = bpp * glFormatElementByteCount(type);

  unsigned datatype;
  if (bpp == 2)
    datatype = GL_UNSIGNED_SHORT;
  else
    datatype = GL_UNSIGNED_BYTE;

  glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / bps);
  glBindTexture(m_textureTarget, plane.id);
  glTexSubImage2D(m_textureTarget, 0, 0, 0, width, height, type, datatype, data);

  /* check if we need to load any border pixels */
  if (height < plane.texheight)
    glTexSubImage2D( m_textureTarget, 0
                   , 0, height, width, 1
                   , type, datatype
                   , (unsigned char*)data + stride * (height-1));

  if (width  < plane.texwidth)
    glTexSubImage2D( m_textureTarget, 0
                   , width, 0, 1, height
                   , type, datatype
                   , (unsigned char*)data + bps * (width-1));

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindTexture(m_textureTarget, 0);
  if (plane.pbo)
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
}

void CLinuxRendererGL::Reset()
{
  for(int i=0; i<m_NumYV12Buffers; i++)
  {
    ReleaseBuffer(i);
  }
}

void CLinuxRendererGL::Flush()
{
  if (!m_bValidated)
    return;

  glFinish();

  for (int i = 0 ; i < m_NumYV12Buffers ; i++)
    DeleteTexture(i);

  glFinish();
  m_bValidated = false;
  m_fbo.fbo.Cleanup();
  m_iYV12RenderBuffer = 0;
}

void CLinuxRendererGL::Update()
{
  if (!m_bConfigured)
    return;
  ManageRenderArea();
  m_scalingMethodGui = (ESCALINGMETHOD)-1;

  ValidateRenderTarget();
}

void CLinuxRendererGL::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  if (index2 >= 0)
    m_iYV12RenderBuffer = index2;
  else
    m_iYV12RenderBuffer = index;

  if (!ValidateRenderer())
  {
    if (clear) //if clear is set, we're expected to overwrite all backbuffer pixels, even if we have nothing to render
      ClearBackBuffer();

    return;
  }

  ManageRenderArea();

  if (clear)
  {
    //draw black bars when video is not transparent, clear the entire backbuffer when it is
    if (alpha == 255)
      DrawBlackBars();
    else
      ClearBackBuffer();
  }

  if (alpha < 255)
  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, alpha / 255.0f);
  }
  else
  {
    glDisable(GL_BLEND);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  }

  if (!Render(flags, m_iYV12RenderBuffer) && clear)
    ClearBackBuffer();

  if (index2 >= 0)
  {
    m_iYV12RenderBuffer = index;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
    Render(flags, m_iYV12RenderBuffer);
  }

  VerifyGLState();
  glEnable(GL_BLEND);
  glFlush();
}

void CLinuxRendererGL::ClearBackBuffer()
{
  //set the entire backbuffer to black
  glClearColor(m_clearColour, m_clearColour, m_clearColour, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(0,0,0,0);
}

//draw black bars around the video quad, this is more efficient than glClear()
//since it only sets pixels to black that aren't going to be overwritten by the video
void CLinuxRendererGL::DrawBlackBars()
{
  glDisable(GL_BLEND);

  struct Svertex
  {
    float x,y,z;
  };
  Svertex vertices[24];
  GLubyte count = 0;

  g_Windowing.EnableShader(SM_DEFAULT);
  GLint posLoc = g_Windowing.ShaderGetPos();
  GLint uniCol = g_Windowing.ShaderGetUniCol();

  glUniform4f(uniCol, m_clearColour / 255.0f, m_clearColour / 255.0f, m_clearColour / 255.0f, 1.0f);

  //top quad
  if (m_rotatedDestCoords[0].y > 0.0)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = 0.0;
    vertices[quad].z = 0;
    vertices[quad+1].x = g_graphicsContext.GetWidth();
    vertices[quad+1].y = 0;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = g_graphicsContext.GetWidth();
    vertices[quad+2].y = m_rotatedDestCoords[0].y;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = m_rotatedDestCoords[0].y;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  //bottom quad
  if (m_rotatedDestCoords[2].y < g_graphicsContext.GetHeight())
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = m_rotatedDestCoords[2].y;
    vertices[quad].z = 0;
    vertices[quad+1].x = g_graphicsContext.GetWidth();
    vertices[quad+1].y = m_rotatedDestCoords[2].y;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = g_graphicsContext.GetWidth();
    vertices[quad+2].y = g_graphicsContext.GetHeight();
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = g_graphicsContext.GetHeight();
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  //left quad
  if (m_rotatedDestCoords[0].x > 0.0)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = m_rotatedDestCoords[0].y;
    vertices[quad].z = 0;
    vertices[quad+1].x = m_rotatedDestCoords[0].x;
    vertices[quad+1].y = m_rotatedDestCoords[0].y;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = m_rotatedDestCoords[3].x;
    vertices[quad+2].y = m_rotatedDestCoords[3].y;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = m_rotatedDestCoords[3].y;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  //right quad
  if (m_rotatedDestCoords[2].x < g_graphicsContext.GetWidth())
  {
    GLubyte quad = count;
    vertices[quad].x = m_rotatedDestCoords[1].x;
    vertices[quad].y = m_rotatedDestCoords[1].y;
    vertices[quad].z = 0;
    vertices[quad+1].x = g_graphicsContext.GetWidth();
    vertices[quad+1].y = m_rotatedDestCoords[1].y;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = g_graphicsContext.GetWidth();
    vertices[quad+2].y = m_rotatedDestCoords[2].y;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = m_rotatedDestCoords[1].x;
    vertices[quad+4].y = m_rotatedDestCoords[2].y;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  GLuint vertexVBO;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Svertex)*count, &vertices[0], GL_STATIC_DRAW);

  glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(Svertex), 0);
  glEnableVertexAttribArray(posLoc);

  glDrawArrays(GL_TRIANGLES, 0, count);

  glDisableVertexAttribArray(posLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);

  g_Windowing.DisableShader();
}

void CLinuxRendererGL::UpdateVideoFilter()
{
  if (!m_pVideoFilterShader)
  {
    m_pVideoFilterShader = new DefaultFilterShader();
    if (!m_pVideoFilterShader->CompileAndLink())
    {
      CLog::Log(LOGERROR, "CLinuxRendererGL::UpdateVideoFilter: Error compiling and linking video filter shader");
      return;
    }
  }

  bool pixelRatioChanged = (CDisplaySettings::GetInstance().GetPixelRatio() > 1.001f ||
                            CDisplaySettings::GetInstance().GetPixelRatio() < 0.999f) !=
                            (m_pixelRatio > 1.001f || m_pixelRatio < 0.999f);
  bool nonLinStretchChanged = false;
  bool cmsChanged = (m_cmsOn != m_ColorManager->IsEnabled()) ||
                    (m_cmsOn && !m_ColorManager->CheckConfiguration(m_cmsToken, m_iFlags));
  if (m_nonLinStretchGui != CDisplaySettings::GetInstance().IsNonLinearStretched() || pixelRatioChanged)
  {
    m_nonLinStretchGui = CDisplaySettings::GetInstance().IsNonLinearStretched();
    m_pixelRatio = CDisplaySettings::GetInstance().GetPixelRatio();
    m_reloadShaders = 1;
    nonLinStretchChanged = true;

    if (m_nonLinStretchGui && (m_pixelRatio < 0.999f || m_pixelRatio > 1.001f) && Supports(RENDERFEATURE_NONLINSTRETCH))
    {
      m_nonLinStretch = true;
      CLog::Log(LOGDEBUG, "GL: Enabling non-linear stretch");
    }
    else
    {
      m_nonLinStretch = false;
      CLog::Log(LOGDEBUG, "GL: Disabling non-linear stretch");
    }
  }

  if (m_scalingMethodGui == CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ScalingMethod &&
      !nonLinStretchChanged && !cmsChanged)
    return;
  else
    m_reloadShaders = 1;

  //recompile YUV shader when non-linear stretch is turned on/off
  //or when it's on and the scaling method changed
  if (m_nonLinStretch || nonLinStretchChanged)
    m_reloadShaders = 1;

  if (cmsChanged)
  {
    if (m_ColorManager->IsEnabled())
    {
      if (!m_ColorManager->CheckConfiguration(m_cmsToken, m_iFlags))
      {
        CLog::Log(LOGDEBUG, "CMS configuration changed, reload LUT");
        LoadCLUT();
      }
      m_cmsOn = true;
    }
    else
    {
      m_cmsOn = false;
    }
  }

  m_scalingMethodGui = CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ScalingMethod;
  m_scalingMethod = m_scalingMethodGui;

  if (!Supports(m_scalingMethod))
  {
    CLog::Log(LOGWARNING, "CLinuxRendererGL::UpdateVideoFilter - chosen scaling method %d, is not supported by renderer", (int)m_scalingMethod);
    m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
  }

  if (m_pVideoFilterShader)
  {
    delete m_pVideoFilterShader;
    m_pVideoFilterShader = nullptr;
  }
  m_fbo.fbo.Cleanup();

  VerifyGLState();

  if (m_scalingMethod == VS_SCALINGMETHOD_AUTO)
  {
    bool scaleSD = m_sourceHeight < 720 && m_sourceWidth < 1280;
    bool scaleUp = (int)m_sourceHeight < g_graphicsContext.GetHeight() && (int)m_sourceWidth < g_graphicsContext.GetWidth();
    bool scaleFps = m_fps < g_advancedSettings.m_videoAutoScaleMaxFps + 0.01f;

    if (Supports(VS_SCALINGMETHOD_LANCZOS3_FAST) && scaleSD && scaleUp && scaleFps)
      m_scalingMethod = VS_SCALINGMETHOD_LANCZOS3_FAST;
    else
      m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
  }

  switch (m_scalingMethod)
  {
  case VS_SCALINGMETHOD_NEAREST:
  case VS_SCALINGMETHOD_LINEAR:
    SetTextureFilter(m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR);
    m_renderQuality = RQ_SINGLEPASS;
    if (Supports(RENDERFEATURE_NONLINSTRETCH) && m_nonLinStretch)
    {
      m_pVideoFilterShader = new StretchFilterShader();
      if (!m_pVideoFilterShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "GL: Error compiling and linking video filter shader");
        break;
      }
    }
    else
    {
      m_pVideoFilterShader = new DefaultFilterShader();
      if (!m_pVideoFilterShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "GL: Error compiling and linking video filter shader");
        break;
      }
    }
    return;

  case VS_SCALINGMETHOD_LANCZOS2:
  case VS_SCALINGMETHOD_SPLINE36_FAST:
  case VS_SCALINGMETHOD_LANCZOS3_FAST:
  case VS_SCALINGMETHOD_SPLINE36:
  case VS_SCALINGMETHOD_LANCZOS3:
  case VS_SCALINGMETHOD_CUBIC:
    if (m_renderMethod & RENDER_GLSL)
    {
      if (!m_fbo.fbo.Initialize())
      {
        CLog::Log(LOGERROR, "GL: Error initializing FBO");
        break;
      }

      if (!m_fbo.fbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_sourceWidth, m_sourceHeight, GL_RGBA16, GL_SHORT))
      {
        CLog::Log(LOGERROR, "GL: Error creating texture and binding to FBO");
        break;
      }
    }

    GLSLOutput *out;
    out = new GLSLOutput(3,
        m_useDithering,
        m_ditherDepth,
        m_cmsOn ? m_fullRange : false,
        m_cmsOn ? m_tCLUTTex : 0,
        m_CLUTsize);
    m_pVideoFilterShader = new ConvolutionFilterShader(m_scalingMethod, m_nonLinStretch, out);
    if (!m_pVideoFilterShader->CompileAndLink())
    {
      CLog::Log(LOGERROR, "GL: Error compiling and linking video filter shader");
      break;
    }

    SetTextureFilter(GL_LINEAR);
    m_renderQuality = RQ_MULTIPASS;
    return;

  case VS_SCALINGMETHOD_BICUBIC_SOFTWARE:
  case VS_SCALINGMETHOD_LANCZOS_SOFTWARE:
  case VS_SCALINGMETHOD_SINC_SOFTWARE:
  case VS_SCALINGMETHOD_SINC8:
  case VS_SCALINGMETHOD_NEDI:
    CLog::Log(LOGERROR, "GL: TODO: This scaler has not yet been implemented");
    break;

  default:
    break;
  }

  CLog::Log(LOGERROR, "GL: Falling back to bilinear due to failure to init scaler");
  if (m_pVideoFilterShader)
  {
    delete m_pVideoFilterShader;
    m_pVideoFilterShader = nullptr;
  }
  m_fbo.fbo.Cleanup();

  m_pVideoFilterShader = new DefaultFilterShader();
  if (!m_pVideoFilterShader->CompileAndLink())
  {
    CLog::Log(LOGERROR, "CLinuxRendererGL::UpdateVideoFilter: Error compiling and linking defauilt video filter shader");
  }

  SetTextureFilter(GL_LINEAR);
  m_renderQuality = RQ_SINGLEPASS;
}

void CLinuxRendererGL::LoadShaders(int field)
{
  m_reloadShaders = 0;

  if (!LoadShadersHook())
  {
    int requestedMethod = CServiceBroker::GetSettings().GetInt(CSettings::SETTING_VIDEOPLAYER_RENDERMETHOD);
    CLog::Log(LOGDEBUG, "GL: Requested render method: %d", requestedMethod);

    if (m_pYUVShader)
    {
      m_pYUVShader->Free();
      delete m_pYUVShader;
      m_pYUVShader = NULL;
    }

    // create regular progressive scan shader
    // if single pass, create GLSLOutput helper and pass it to YUV2RGB shader
    GLSLOutput *out = nullptr;
    if (m_renderQuality == RQ_SINGLEPASS)
    {
      out = new GLSLOutput(3, m_useDithering, m_ditherDepth,
                           m_cmsOn ? m_fullRange : false,
                           m_cmsOn ? m_tCLUTTex : 0,
                           m_CLUTsize);
    }
    EShaderFormat shaderFormat = GetShaderFormat();
    m_pYUVShader = new YUV2RGBProgressiveShader(m_textureTarget == GL_TEXTURE_RECTANGLE_ARB, m_iFlags, shaderFormat,
                                                m_nonLinStretch && m_renderQuality == RQ_SINGLEPASS,
                                                out);
    if (!m_cmsOn)
      m_pYUVShader->SetConvertFullColorRange(m_fullRange);

    CLog::Log(LOGNOTICE, "GL: Selecting YUV 2 RGB shader");

    if (m_pYUVShader && m_pYUVShader->CompileAndLink())
    {
      m_renderMethod = RENDER_GLSL;
      UpdateVideoFilter();
    }
    else
    {
      CLog::Log(LOGERROR, "GL: Error enabling YUV2RGB GLSL shader");
    }
  }

  // determine whether GPU supports NPOT textures
  if (!g_Windowing.IsExtSupported("GL_ARB_texture_non_power_of_two"))
  {
    if (!g_Windowing.IsExtSupported("GL_ARB_texture_rectangle"))
    {
      CLog::Log(LOGWARNING, "GL: GL_ARB_texture_rectangle not supported and OpenGL version is not 2.x");
    }
    else
      CLog::Log(LOGNOTICE, "GL: NPOT textures are supported through GL_ARB_texture_rectangle extension");
  }
  else
    CLog::Log(LOGNOTICE, "GL: NPOT texture support detected");

  
  if (m_pboSupported)
  {
    CLog::Log(LOGNOTICE, "GL: Using GL_ARB_pixel_buffer_object");
    m_pboUsed = true;
  }
  else
    m_pboUsed = false;
}

void CLinuxRendererGL::UnInit()
{
  CLog::Log(LOGDEBUG, "LinuxRendererGL: Cleaning up GL resources");
  CSingleLock lock(g_graphicsContext);

  glFinish();

  // YV12 textures
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    DeleteTexture(i);
  }

  DeleteCLUT();

  // cleanup framebuffer object if it was in use
  m_fbo.fbo.Cleanup();
  m_bValidated = false;
  m_bConfigured = false;
}

bool CLinuxRendererGL::Render(DWORD flags, int renderBuffer)
{
  // obtain current field, if interlaced
  if( flags & RENDER_FLAG_TOP)
    m_currentField = FIELD_TOP;

  else if (flags & RENDER_FLAG_BOT)
    m_currentField = FIELD_BOT;

  else
    m_currentField = FIELD_FULL;

  // call texture load function
  if (!UploadTexture(renderBuffer))
  {
    return false;
  }

  if (RenderHook(renderBuffer))
    ;
  else if (m_renderMethod & RENDER_GLSL)
  {
    UpdateVideoFilter();
    switch(m_renderQuality)
    {
    case RQ_LOW:
    case RQ_SINGLEPASS:
      RenderSinglePass(renderBuffer, m_currentField);
      VerifyGLState();
      break;

    case RQ_MULTIPASS:
      RenderToFBO(renderBuffer, m_currentField);
      RenderFromFBO();
      VerifyGLState();
      break;
    }
  }
  else
  {
    return false;
  }

  AfterRenderHook(renderBuffer);
  return true;
}

void CLinuxRendererGL::RenderSinglePass(int index, int field)
{
  YUVPLANE (&planes)[YuvImage::MAX_PLANES] = m_buffers[index].fields[field];

  if (m_reloadShaders)
  {
    LoadShaders(field);
  }

  glDisable(GL_DEPTH_TEST);

  // Y
  glActiveTextureARB(GL_TEXTURE0);
  glBindTexture(m_textureTarget, planes[0].id);

  // U
  glActiveTextureARB(GL_TEXTURE1);
  glBindTexture(m_textureTarget, planes[1].id);

  // V
  glActiveTextureARB(GL_TEXTURE2);
  glBindTexture(m_textureTarget, planes[2].id);

  glActiveTextureARB(GL_TEXTURE0);
  VerifyGLState();

  m_pYUVShader->SetBlack(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Brightness * 0.01f - 0.5f);
  m_pYUVShader->SetContrast(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Contrast * 0.02f);
  m_pYUVShader->SetWidth(planes[0].texwidth);
  m_pYUVShader->SetHeight(planes[0].texheight);

  //disable non-linear stretch when a dvd menu is shown, parts of the menu are rendered through the overlay renderer
  //having non-linear stretch on breaks the alignment
  if (g_application.m_pPlayer->IsInMenu())
    m_pYUVShader->SetNonLinStretch(1.0);
  else
    m_pYUVShader->SetNonLinStretch(pow(CDisplaySettings::GetInstance().GetPixelRatio(), g_advancedSettings.m_videoNonLinStretchRatio));

  if (field == FIELD_TOP)
    m_pYUVShader->SetField(1);
  else if(field == FIELD_BOT)
    m_pYUVShader->SetField(0);

  m_pYUVShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  m_pYUVShader->Enable();

  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLuint vertexVBO;
  GLuint indexVBO;

  struct PackedVertex
  {
    float x, y, z;
    float u1, v1;
    float u2, v2;
    float u3, v3;
  }vertex[4];

  GLint vertLoc = m_pYUVShader->GetVertexLoc();
  GLint Yloc = m_pYUVShader->GetYcoordLoc();
  GLint Uloc = m_pYUVShader->GetUcoordLoc();
  GLint Vloc = m_pYUVShader->GetVcoordLoc();

  // Setup vertex position values
  for(int i = 0; i < 4; i++)
  {
    vertex[i].x = m_rotatedDestCoords[i].x;
    vertex[i].y = m_rotatedDestCoords[i].y;
    vertex[i].z = 0.0f;// set z to 0
  }

  // bottom left
  vertex[0].u1 = planes[0].rect.x1;
  vertex[0].v1 = planes[0].rect.y1;
  vertex[0].u2 = planes[1].rect.x1;
  vertex[0].v2 = planes[1].rect.y1;
  vertex[0].u3 = planes[2].rect.x1;
  vertex[0].v3 = planes[2].rect.y1;

  // bottom right
  vertex[1].u1 = planes[0].rect.x2;
  vertex[1].v1 = planes[0].rect.y1;
  vertex[1].u2 = planes[1].rect.x2;
  vertex[1].v2 = planes[1].rect.y1;
  vertex[1].u3 = planes[2].rect.x2;
  vertex[1].v3 = planes[2].rect.y1;

  // top right
  vertex[2].u1 = planes[0].rect.x2;
  vertex[2].v1 = planes[0].rect.y2;
  vertex[2].u2 = planes[1].rect.x2;
  vertex[2].v2 = planes[1].rect.y2;
  vertex[2].u3 = planes[2].rect.x2;
  vertex[2].v3 = planes[2].rect.y2;

  // top left
  vertex[3].u1 = planes[0].rect.x1;
  vertex[3].v1 = planes[0].rect.y2;
  vertex[3].u2 = planes[1].rect.x1;
  vertex[3].v2 = planes[1].rect.y2;
  vertex[3].u3 = planes[2].rect.x1;
  vertex[3].v3 = planes[2].rect.y2;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex)*4, &vertex[0], GL_STATIC_DRAW);

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, x)));
  glVertexAttribPointer(Yloc, 2, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, u1)));
  glVertexAttribPointer(Uloc, 2, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, u2)));
  glVertexAttribPointer(Vloc, 2, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, u3)));

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(Yloc);
  glEnableVertexAttribArray(Uloc);
  glEnableVertexAttribArray(Vloc);

  glGenBuffers(1, &indexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexVBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte)*4, idx, GL_STATIC_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, 0);
  VerifyGLState();

  glDisableVertexAttribArray(vertLoc);
  glDisableVertexAttribArray(Yloc);
  glDisableVertexAttribArray(Uloc);
  glDisableVertexAttribArray(Vloc);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &indexVBO);

  m_pYUVShader->Disable();
  VerifyGLState();

  VerifyGLState();
}

void CLinuxRendererGL::RenderToFBO(int index, int field, bool weave /*= false*/)
{
  YUVPLANE (&planes)[YuvImage::MAX_PLANES] = m_buffers[index].fields[field];

  if (m_reloadShaders)
  {
    m_reloadShaders = 0;
    LoadShaders(m_currentField);
  }

  if (!m_fbo.fbo.IsValid())
  {
    if (!m_fbo.fbo.Initialize())
    {
      CLog::Log(LOGERROR, "GL: Error initializing FBO");
      return;
    }

    if (!m_fbo.fbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_sourceWidth, m_sourceHeight, GL_RGBA, GL_SHORT))
    {
      CLog::Log(LOGERROR, "GL: Error creating texture and binding to FBO");
      return;
    }
  }

  glDisable(GL_DEPTH_TEST);

  // Y
  glActiveTextureARB(GL_TEXTURE0);
  glBindTexture(m_textureTarget, planes[0].id);
  VerifyGLState();

  // U
  glActiveTextureARB(GL_TEXTURE1);
  glBindTexture(m_textureTarget, planes[1].id);
  VerifyGLState();

  // V
  glActiveTextureARB(GL_TEXTURE2);
  glBindTexture(m_textureTarget, planes[2].id);
  VerifyGLState();

  glActiveTextureARB(GL_TEXTURE0);
  VerifyGLState();

  // make sure the yuv shader is loaded and ready to go
  if (!m_pYUVShader || (!m_pYUVShader->OK()))
  {
    CLog::Log(LOGERROR, "GL: YUV shader not active, cannot do multipass render");
    return;
  }

  m_fbo.fbo.BeginRender();
  VerifyGLState();

  m_pYUVShader->SetBlack(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Brightness * 0.01f - 0.5f);
  m_pYUVShader->SetContrast(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Contrast * 0.02f);
  m_pYUVShader->SetWidth(planes[0].texwidth);
  m_pYUVShader->SetHeight(planes[0].texheight);
  m_pYUVShader->SetNonLinStretch(1.0);

  if (field == FIELD_TOP)
    m_pYUVShader->SetField(1);
  else if (field == FIELD_BOT)
    m_pYUVShader->SetField(0);

  VerifyGLState();

  glMatrixModview.Push();
  glMatrixModview->LoadIdentity();
  glMatrixModview.Load();

  glMatrixProject.Push();
  glMatrixProject->LoadIdentity();
  glMatrixProject->Ortho2D(0, m_sourceWidth, 0, m_sourceHeight);
  glMatrixProject.Load();

  CRect viewport;
  g_Windowing.GetViewPort(viewport);
  glViewport(0, 0, m_sourceWidth, m_sourceHeight);
  glScissor (0, 0, m_sourceWidth, m_sourceHeight);

  m_pYUVShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  if (!m_pYUVShader->Enable())
  {
    CLog::Log(LOGERROR, "GL: Error enabling YUV shader");
  }

  m_fbo.width = planes[0].rect.x2 - planes[0].rect.x1;
  m_fbo.height = planes[0].rect.y2 - planes[0].rect.y1;

  if (m_textureTarget == GL_TEXTURE_2D)
  {
    m_fbo.width  *= planes[0].texwidth;
    m_fbo.height *= planes[0].texheight;
  }
  m_fbo.width  *= planes[0].pixpertex_x;
  m_fbo.height *= planes[0].pixpertex_y;
  if (weave)
    m_fbo.height *= 2;

  // 1st Pass to video frame size
  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLuint vertexVBO;
  GLuint indexVBO;
  struct PackedVertex
  {
    float x, y, z;
    float u1, v1;
    float u2, v2;
    float u3, v3;
  } vertex[4];

  GLint vertLoc = m_pYUVShader->GetVertexLoc();
  GLint Yloc = m_pYUVShader->GetYcoordLoc();
  GLint Uloc = m_pYUVShader->GetUcoordLoc();
  GLint Vloc = m_pYUVShader->GetVcoordLoc();

  // top left
  vertex[0].x = 0.0f;
  vertex[0].y = 0.0f;
  vertex[0].z = 0.0f;
  vertex[0].u1 = planes[0].rect.x1;
  vertex[0].v1 = planes[0].rect.y1;
  vertex[0].u2 = planes[1].rect.x1;
  vertex[0].v2 = planes[1].rect.y1;
  vertex[0].u3 = planes[2].rect.x1;
  vertex[0].v3 = planes[2].rect.y1;

  // top right
  vertex[1].x = m_fbo.width;
  vertex[1].y = 0.0f;
  vertex[1].z = 0.0f;
  vertex[1].u1 = planes[0].rect.x2;
  vertex[1].v1 = planes[0].rect.y1;
  vertex[1].u2 = planes[1].rect.x2;
  vertex[1].v2 = planes[1].rect.y1;
  vertex[1].u3 = planes[2].rect.x2;
  vertex[1].v3 = planes[2].rect.y1;

  // bottom right
  vertex[2].x = m_fbo.width;
  vertex[2].y = m_fbo.height;
  vertex[2].z = 0.0f;
  vertex[2].u1 = planes[0].rect.x2;
  vertex[2].v1 = planes[0].rect.y2;
  vertex[2].u2 = planes[1].rect.x2;
  vertex[2].v2 = planes[1].rect.y2;
  vertex[2].u3 = planes[2].rect.x2;
  vertex[2].v3 = planes[2].rect.y2;

  // bottom left
  vertex[3].x = 0.0f;
  vertex[3].y = m_fbo.height;
  vertex[3].z = 0.0f;
  vertex[3].u1 = planes[0].rect.x1;
  vertex[3].v1 = planes[0].rect.y2;
  vertex[3].u2 = planes[1].rect.x1;
  vertex[3].v2 = planes[1].rect.y2;
  vertex[3].u3 = planes[2].rect.x1;
  vertex[3].v3 = planes[2].rect.y2;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex)*4, &vertex[0], GL_STATIC_DRAW);

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, x)));
  glVertexAttribPointer(Yloc, 2, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, u1)));
  glVertexAttribPointer(Uloc, 2, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, u2)));
  glVertexAttribPointer(Vloc, 2, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, u3)));

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(Yloc);
  glEnableVertexAttribArray(Uloc);
  glEnableVertexAttribArray(Vloc);

  glGenBuffers(1, &indexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexVBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte)*4, idx, GL_STATIC_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, 0);
  VerifyGLState();

  glDisableVertexAttribArray(vertLoc);
  glDisableVertexAttribArray(Yloc);
  glDisableVertexAttribArray(Uloc);
  glDisableVertexAttribArray(Vloc);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &indexVBO);

  m_pYUVShader->Disable();

  glMatrixModview.PopLoad();
  glMatrixProject.PopLoad();

  g_Windowing.SetViewPort(viewport);

  m_fbo.fbo.EndRender();

  VerifyGLState();
}

void CLinuxRendererGL::RenderFromFBO()
{
  glActiveTextureARB(GL_TEXTURE0);
  VerifyGLState();

  // Use regular normalized texture coordinates
  // 2nd Pass to screen size with optional video filter

  if (!m_pVideoFilterShader)
  {
    CLog::Log(LOGERROR, "CLinuxRendererGL::RenderFromFBO - no videofilter shader");
    return;
  }

  GLint filter;
  if (!m_pVideoFilterShader->GetTextureFilter(filter))
    filter = m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;

  m_fbo.fbo.SetFiltering(GL_TEXTURE_2D, filter);
  m_pVideoFilterShader->SetSourceTexture(0);
  m_pVideoFilterShader->SetWidth(m_sourceWidth);
  m_pVideoFilterShader->SetHeight(m_sourceHeight);

  //disable non-linear stretch when a dvd menu is shown, parts of the menu are rendered through the overlay renderer
  //having non-linear stretch on breaks the alignment
  if (g_application.m_pPlayer->IsInMenu())
    m_pVideoFilterShader->SetNonLinStretch(1.0);
  else
    m_pVideoFilterShader->SetNonLinStretch(pow(CDisplaySettings::GetInstance().GetPixelRatio(), g_advancedSettings.m_videoNonLinStretchRatio));

  m_pVideoFilterShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  m_pVideoFilterShader->Enable();

  VerifyGLState();

  float imgwidth = m_fbo.width / m_sourceWidth;
  float imgheight = m_fbo.height / m_sourceHeight;

  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLuint vertexVBO;
  GLuint indexVBO;
  struct PackedVertex
  {
    float x, y, z;
    float u1, v1;
  } vertex[4];

  GLint vertLoc = m_pVideoFilterShader->GetVertexLoc();
  GLint loc = m_pVideoFilterShader->GetCoordLoc();

  // Setup vertex position values
  // top left
  vertex[0].x = m_rotatedDestCoords[0].x;
  vertex[0].y = m_rotatedDestCoords[0].y;
  vertex[0].z = 0.0f;
  vertex[0].u1 = 0.0;
  vertex[0].v1 = 0.0;

  // top right
  vertex[1].x = m_rotatedDestCoords[1].x;
  vertex[1].y = m_rotatedDestCoords[1].y;
  vertex[1].z = 0.0f;
  vertex[1].u1 = imgwidth;
  vertex[1].v1 = 0.0f;

  // bottom right
  vertex[2].x = m_rotatedDestCoords[2].x;
  vertex[2].y = m_rotatedDestCoords[2].y;
  vertex[2].z = 0.0f;
  vertex[2].u1 = imgwidth;
  vertex[2].v1 = imgheight;

  // bottom left
  vertex[3].x = m_rotatedDestCoords[3].x;
  vertex[3].y = m_rotatedDestCoords[3].y;
  vertex[3].z = 0.0f;
  vertex[3].u1 = 0.0f;
  vertex[3].v1 = imgheight;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex)*4, &vertex[0], GL_STATIC_DRAW);
  
  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, x)));
  glVertexAttribPointer(loc, 2, GL_FLOAT, 0, sizeof(PackedVertex), BUFFER_OFFSET(offsetof(PackedVertex, u1)));

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(loc);

  glGenBuffers(1, &indexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexVBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte)*4, idx, GL_STATIC_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, 0);
  VerifyGLState();

  glDisableVertexAttribArray(loc);
  glDisableVertexAttribArray(vertLoc);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &indexVBO);

  m_pVideoFilterShader->Disable();

  VerifyGLState();

  glBindTexture(GL_TEXTURE_2D, 0);
  VerifyGLState();
}

void CLinuxRendererGL::RenderProgressiveWeave(int index, int field)
{
  bool scale = (int)m_sourceHeight != m_destRect.Height() ||
               (int)m_sourceWidth != m_destRect.Width();

  if (m_fbo.fbo.IsSupported() && (scale || m_renderQuality == RQ_MULTIPASS))
  {
    glEnable(GL_POLYGON_STIPPLE);
    glPolygonStipple(stipple_weave);
    RenderToFBO(index, FIELD_TOP, true);
    glPolygonStipple(stipple_weave+4);
    RenderToFBO(index, FIELD_BOT, true);
    glDisable(GL_POLYGON_STIPPLE);
    RenderFromFBO();
  }
  else
  {
    glEnable(GL_POLYGON_STIPPLE);
    glPolygonStipple(stipple_weave);
    RenderSinglePass(index, FIELD_TOP);
    glPolygonStipple(stipple_weave+4);
    RenderSinglePass(index, FIELD_BOT);
    glDisable(GL_POLYGON_STIPPLE);
  }
}

void CLinuxRendererGL::RenderRGB(int index, int field)
{
  YUVPLANE &plane = m_buffers[index].fields[FIELD_FULL][0];

  glActiveTextureARB(GL_TEXTURE0);

  glBindTexture(m_textureTarget, plane.id);

  // make sure we know the correct texture size
  GetPlaneTextureSize(plane);

  if (!m_pVideoFilterShader)
  {
    CLog::Log(LOGERROR, "CLinuxRendererGL::RenderRGB - no videofilter shader");
    return;
  }

  GLint filter;
  if (!m_pVideoFilterShader->GetTextureFilter(filter))
    filter = m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;

  glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, filter);
  m_pVideoFilterShader->SetSourceTexture(0);
  m_pVideoFilterShader->SetWidth(m_sourceWidth);
  m_pVideoFilterShader->SetHeight(m_sourceHeight);

  //disable non-linear stretch when a dvd menu is shown, parts of the menu are rendered through the overlay renderer
  //having non-linear stretch on breaks the alignment
  if (g_application.m_pPlayer->IsInMenu())
    m_pVideoFilterShader->SetNonLinStretch(1.0);
  else
    m_pVideoFilterShader->SetNonLinStretch(pow(CDisplaySettings::GetInstance().GetPixelRatio(), g_advancedSettings.m_videoNonLinStretchRatio));

  m_pVideoFilterShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  m_pVideoFilterShader->Enable();

  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLfloat vertex[4][3];
  GLfloat texture[4][2];

  GLint vertLoc = m_pVideoFilterShader->GetVertexLoc();
  GLint loc = m_pVideoFilterShader->GetCoordLoc();

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, 0, vertex);
  glVertexAttribPointer(loc, 2, GL_FLOAT, 0, 0, texture);

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(loc);

  // Setup vertex position values
  vertex[0][0] = m_rotatedDestCoords[0].x;
  vertex[0][1] = m_rotatedDestCoords[0].y;
  vertex[0][2] = 0.0f;

  vertex[1][0] = m_rotatedDestCoords[1].x;
  vertex[1][1] = m_rotatedDestCoords[1].y;
  vertex[1][2] = 0.0f;

  vertex[2][0] = m_rotatedDestCoords[2].x;
  vertex[2][1] = m_rotatedDestCoords[2].y;
  vertex[2][2] = 0.0f;

  vertex[3][0] = m_rotatedDestCoords[3].x;
  vertex[3][1] = m_rotatedDestCoords[3].y;
  vertex[3][2] = 0.0f;

  texture[0][0] = plane.rect.x1;
  texture[0][1] = plane.rect.y1;
  texture[1][0] = plane.rect.x2;
  texture[1][1] = plane.rect.y1;
  texture[2][0] = plane.rect.x2;
  texture[2][1] = plane.rect.y2;
  texture[3][0] = plane.rect.x1;
  texture[3][1] = plane.rect.y2;

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);
  VerifyGLState();

  m_pVideoFilterShader->Disable();

  glBindTexture(m_textureTarget, 0);
}

bool CLinuxRendererGL::RenderCapture(CRenderCapture* capture)
{
  if (!m_bValidated)
    return false;

  // save current video rect
  CRect saveSize = m_destRect;
  
  saveRotatedCoords();//backup current m_rotatedDestCoords
      
  // new video rect is capture size
  m_destRect.SetRect(0, 0, (float)capture->GetWidth(), (float)capture->GetHeight());
  MarkDirty();
  syncDestRectToRotatedPoints();//syncs the changed destRect to m_rotatedDestCoords

  //invert Y axis to get non-inverted image
  glDisable(GL_BLEND);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  glMatrixModview.Push();
  glMatrixModview->Translatef(0.0f, capture->GetHeight(), 0.0f);
  glMatrixModview->Scalef(1.0f, -1.0f, 1.0f);
  glMatrixModview.Load();

  capture->BeginRender();

  Render(RENDER_FLAG_NOOSD, m_iYV12RenderBuffer);
  // read pixels
  glReadPixels(0, g_graphicsContext.GetHeight() - capture->GetHeight(), capture->GetWidth(), capture->GetHeight(),
               GL_BGRA, GL_UNSIGNED_BYTE, capture->GetRenderBuffer());

  capture->EndRender();

  // revert model view matrix
  glMatrixModview.PopLoad();

  // restore original video rect
  m_destRect = saveSize;
  restoreRotatedCoords();//restores the previous state of the rotated dest coords

  return true;
}


static GLint GetInternalFormat(GLint format, int bpp)
{
  if (bpp == 2)
  {
    switch (format)
    {
#ifdef GL_R16
      case GL_RED:
        return GL_R16;
#endif
      default:
        return format;
    }
  }
  else
    return format;
}

//-----------------------------------------------------------------------------
// Textures
//-----------------------------------------------------------------------------

bool CLinuxRendererGL::CreateTexture(int index)
{
  if (m_format == AV_PIX_FMT_NV12)
    return CreateNV12Texture(index);
  else if (m_format == AV_PIX_FMT_YUYV422 ||
           m_format == AV_PIX_FMT_UYVY422)
    return CreateYUV422PackedTexture(index);
  else
    return CreateYV12Texture(index);
}

void CLinuxRendererGL::DeleteTexture(int index)
{
  ReleaseBuffer(index);

  if (m_format == AV_PIX_FMT_NV12)
    DeleteNV12Texture(index);
  else if (m_format == AV_PIX_FMT_YUYV422 ||
           m_format == AV_PIX_FMT_UYVY422)
    DeleteYUV422PackedTexture(index);
  else
    DeleteYV12Texture(index);
}

bool CLinuxRendererGL::UploadTexture(int index)
{
  if (!m_buffers[index].videoBuffer)
    return false;

  if (m_buffers[index].loaded)
    return true;

  bool ret = false;

  YuvImage &dst = m_buffers[index].image;
  YuvImage src;
  m_buffers[index].videoBuffer->GetPlanes(src.plane);
  m_buffers[index].videoBuffer->GetStrides(src.stride);

  UnBindPbo(m_buffers[index]);

  if (m_format == AV_PIX_FMT_NV12)
  {
    CVideoBuffer::CopyNV12Picture(&dst, &src);
    BindPbo(m_buffers[index]);
    ret = UploadNV12Texture(index);
  }
  else if (m_format == AV_PIX_FMT_YUYV422 ||
           m_format == AV_PIX_FMT_UYVY422)
  {
    CVideoBuffer::CopyYUV422PackedPicture(&dst, &src);
    BindPbo(m_buffers[index]);
    ret = UploadYUV422PackedTexture(index);
  }
  else
  {
    CVideoBuffer::CopyPicture(&dst, &src);
    BindPbo(m_buffers[index]);
    ret = UploadYV12Texture(index);
  }

  if (ret)
    m_buffers[index].loaded = true;

  return ret;
}

//********************************************************************************************************
// YV12 Texture creation, deletion, copying + clearing
//********************************************************************************************************

bool CLinuxRendererGL::CreateYV12Texture(int index)
{
  /* since we also want the field textures, pitch must be texture aligned */
  unsigned p;

  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DeleteYV12Texture(index);

  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;

  if(m_format == AV_PIX_FMT_YUV420P16 ||
     m_format == AV_PIX_FMT_YUV420P14 ||
     m_format == AV_PIX_FMT_YUV420P12 ||
     m_format == AV_PIX_FMT_YUV420P10 ||
     m_format == AV_PIX_FMT_YUV420P9)
    im.bpp = 2;
  else
    im.bpp = 1;

  im.stride[0] = im.bpp * im.width;
  im.stride[1] = im.bpp * (im.width >> im.cshift_x);
  im.stride[2] = im.bpp * (im.width >> im.cshift_x);

  im.planesize[0] = im.stride[0] * im.height;
  im.planesize[1] = im.stride[1] * (im.height >> im.cshift_y);
  im.planesize[2] = im.stride[2] * (im.height >> im.cshift_y);

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffersARB(3, pbo);

    for (int i = 0; i < 3; i++)
    {
      glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[i]);
      glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, im.planesize[i] + PBO_OFFSET, 0, GL_STREAM_DRAW_ARB);
      void* pboPtr = glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
      if (pboPtr)
      {
        im.plane[i] = (uint8_t*) pboPtr + PBO_OFFSET;
        memset(im.plane[i], 0, im.planesize[i]);
      }
      else
      {
        CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
        pboSetup = false;
        break;
      }
    }

    if (!pboSetup)
    {
      for (int i = 0; i < 3; i++)
      {
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[i]);
        glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
      }
      glDeleteBuffersARB(3, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
  }

  if (!pboSetup)
  {
    for (int i = 0; i < 3; i++)
      im.plane[i] = new uint8_t[im.planesize[i]];
  }

  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(p = 0;p<YuvImage::MAX_PLANES;p++)
    {
      if (!glIsTexture(m_buffers[index].fields[f][p].id))
      {
        glGenTextures(1, &m_buffers[index].fields[f][p].id);
        VerifyGLState();
      }
      m_buffers[index].fields[f][p].pbo = pbo[p];
    }
  }

  // YUV
  for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
  {
    int fieldshift = (f==FIELD_FULL) ? 0 : 1;
    YUVPLANE (&planes)[YuvImage::MAX_PLANES] = m_buffers[index].fields[f];

    planes[0].texwidth  = im.width;
    planes[0].texheight = im.height >> fieldshift;

    planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[1].texheight = planes[0].texheight >> im.cshift_y;
    planes[2].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[2].texheight = planes[0].texheight >> im.cshift_y;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 1;
      planes[p].pixpertex_y = 1;
    }

    for (int p = 0; p < 3; p++)
    {
      YUVPLANE &plane = planes[p];
      if (plane.texwidth * plane.texheight == 0)
        continue;

      glBindTexture(m_textureTarget, plane.id);
      GLint internalformat;
      internalformat = GetInternalFormat(GL_RED, im.bpp);
      glTexImage2D(m_textureTarget, 0, internalformat, plane.texwidth, plane.texheight, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

      glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      VerifyGLState();
    }
  }
  return true;
}

bool CLinuxRendererGL::UploadYV12Texture(int source)
{
  YUVBUFFER& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  bool deinterlacing;
  if (m_currentField == FIELD_FULL)
    deinterlacing = false;
  else
    deinterlacing = true;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT,1);

  if (deinterlacing)
  {
    // Load Even Y Field
    LoadPlane(buf.fields[FIELD_TOP][0] , GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0] );

    //load Odd Y Field
    LoadPlane(buf.fields[FIELD_BOT][0], GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0] + im->stride[0]);

    // Load Even U & V Fields
    LoadPlane(buf.fields[FIELD_TOP][1], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1]);

    LoadPlane(buf.fields[FIELD_TOP][2], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[2]*2, im->bpp, im->plane[2]);

    // Load Odd U & V Fields
    LoadPlane(buf.fields[FIELD_BOT][1], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1] + im->stride[1]);

    LoadPlane(buf.fields[FIELD_BOT][2], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[2]*2, im->bpp, im->plane[2] + im->stride[2]);
  }
  else
  {
    //Load Y plane
    LoadPlane(buf.fields[FIELD_FULL][0], GL_RED,
              im->width, im->height,
              im->stride[0], im->bpp, im->plane[0]);

    //load U plane
    LoadPlane(buf.fields[FIELD_FULL][1], GL_RED,
              im->width >> im->cshift_x, im->height >> im->cshift_y,
              im->stride[1], im->bpp, im->plane[1]);

    //load V plane
    LoadPlane(buf.fields[FIELD_FULL][2], GL_RED,
              im->width >> im->cshift_x, im->height >> im->cshift_y,
              im->stride[2], im->bpp, im->plane[2]);
  }

  VerifyGLState();

  CalculateTextureSourceRects(source, 3);

  return true;
}

void CLinuxRendererGL::DeleteYV12Texture(int index)
{
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  if (m_buffers[index].fields[FIELD_FULL][0].id == 0)
    return;

  /* finish up all textures, and delete them */
  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(int p = 0;p<YuvImage::MAX_PLANES;p++)
    {
      if (m_buffers[index].fields[f][p].id)
      {
        if (glIsTexture(m_buffers[index].fields[f][p].id))
          glDeleteTextures(1, &m_buffers[index].fields[f][p].id);
        m_buffers[index].fields[f][p].id = 0;
      }
    }
  }

  for(int p = 0;p<YuvImage::MAX_PLANES;p++)
  {
    if (pbo[p])
    {
      if (im.plane[p])
      {
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[p]);
        glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
        im.plane[p] = NULL;
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
      }
      glDeleteBuffersARB(1, pbo + p);
      pbo[p] = 0;
    }
    else
    {
      if (im.plane[p])
      {
        delete[] im.plane[p];
        im.plane[p] = NULL;
      }
    }
  }
}

//********************************************************************************************************
// NV12 Texture loading, creation and deletion
//********************************************************************************************************
bool CLinuxRendererGL::UploadNV12Texture(int source)
{
  YUVBUFFER& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  bool deinterlacing;
  if (m_currentField == FIELD_FULL)
    deinterlacing = false;
  else
    deinterlacing = true;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT, im->bpp);

  if (deinterlacing)
  {
    // Load Odd Y field
    LoadPlane(buf.fields[FIELD_TOP][0] , GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0]);

    // Load Even Y field
    LoadPlane(buf.fields[FIELD_BOT][0], GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0] + im->stride[0]) ;

    // Load Odd UV Fields
    LoadPlane(buf.fields[FIELD_TOP][1], GL_RG,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1]);

    // Load Even UV Fields
    LoadPlane(buf.fields[FIELD_BOT][1], GL_RG,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1] + im->stride[1]);

  }
  else
  {
    // Load Y plane
    LoadPlane(buf. fields[FIELD_FULL][0], GL_RED,
              im->width, im->height,
              im->stride[0], im->bpp, im->plane[0]);

    // Load UV plane
    LoadPlane(buf.fields[FIELD_FULL][1], GL_RG,
              im->width >> im->cshift_x, im->height >> im->cshift_y,
              im->stride[1], im->bpp, im->plane[1]);
  }

  VerifyGLState();

  CalculateTextureSourceRects(source, 3);

  return true;
}

bool CLinuxRendererGL::CreateNV12Texture(int index)
{
  // since we also want the field textures, pitch must be texture aligned
  YUVBUFFER& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

  // Delete any old texture
  DeleteNV12Texture(index);

  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;
  im.bpp = 1;

  im.stride[0] = im.width;
  im.stride[1] = im.width;
  im.stride[2] = 0;

  im.plane[0] = NULL;
  im.plane[1] = NULL;
  im.plane[2] = NULL;

  // Y plane
  im.planesize[0] = im.stride[0] * im.height;
  // packed UV plane
  im.planesize[1] = im.stride[1] * im.height / 2;
  // third plane is not used
  im.planesize[2] = 0;

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffersARB(2, pbo);

    for (int i = 0; i < 2; i++)
    {
      glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[i]);
      glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, im.planesize[i] + PBO_OFFSET, 0, GL_STREAM_DRAW_ARB);
      void* pboPtr = glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
      if (pboPtr)
      {
        im.plane[i] = (uint8_t*)pboPtr + PBO_OFFSET;
        memset(im.plane[i], 0, im.planesize[i]);
      }
      else
      {
        CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
        pboSetup = false;
        break;
      }
    }

    if (!pboSetup)
    {
      for (int i = 0; i < 2; i++)
      {
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[i]);
        glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
      }
      glDeleteBuffersARB(2, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
  }

  if (!pboSetup)
  {
    for (int i = 0; i < 2; i++)
      im.plane[i] = new uint8_t[im.planesize[i]];
  }

  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(int p = 0;p<2;p++)
    {
      if (!glIsTexture(buf.fields[f][p].id))
      {
        glGenTextures(1, &buf.fields[f][p].id);
        VerifyGLState();
      }
      buf.fields[f][p].pbo = pbo[p];
    }
    buf.fields[f][2].id = buf.fields[f][1].id;
  }

  // YUV
  for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
  {
    int fieldshift = (f==FIELD_FULL) ? 0 : 1;
    YUVPLANE (&planes)[YuvImage::MAX_PLANES] = buf.fields[f];

    planes[0].texwidth  = im.width;
    planes[0].texheight = im.height >> fieldshift;

    planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[1].texheight = planes[0].texheight >> im.cshift_y;
    planes[2].texwidth  = planes[1].texwidth;
    planes[2].texheight = planes[1].texheight;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 1;
      planes[p].pixpertex_y = 1;
    }

    for(int p = 0; p < 2; p++)
    {
      YUVPLANE &plane = planes[p];
      if (plane.texwidth * plane.texheight == 0)
        continue;

      glBindTexture(m_textureTarget, plane.id);
      if (p == 1)
        glTexImage2D(m_textureTarget, 0, GL_LUMINANCE_ALPHA, plane.texwidth, plane.texheight, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
      else
        glTexImage2D(m_textureTarget, 0, GL_LUMINANCE, plane.texwidth, plane.texheight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

      glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      VerifyGLState();
    }
  }

  return true;
}

void CLinuxRendererGL::DeleteNV12Texture(int index)
{
  YUVBUFFER& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

  if (buf.fields[FIELD_FULL][0].id == 0)
    return;

  // finish up all textures, and delete them
  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(int p = 0;p<2;p++)
    {
      if (buf.fields[f][p].id)
      {
        if (glIsTexture(buf.fields[f][p].id))
        {
          glDeleteTextures(1, &buf.fields[f][p].id);
        }
        buf.fields[f][p].id = 0;
      }
    }
    buf.fields[f][2].id = 0;
  }

  for(int p = 0;p<2;p++)
  {
    if (pbo[p])
    {
      if (im.plane[p])
      {
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[p]);
        glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
        im.plane[p] = NULL;
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
      }
      glDeleteBuffersARB(1, pbo + p);
      pbo[p] = 0;
    }
    else
    {
      if (im.plane[p])
      {
        delete[] im.plane[p];
        im.plane[p] = NULL;
      }
    }
  }
}

bool CLinuxRendererGL::UploadYUV422PackedTexture(int source)
{
  YUVBUFFER& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  bool deinterlacing;
  if (m_currentField == FIELD_FULL)
    deinterlacing = false;
  else
    deinterlacing = true;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT,1);

  if (deinterlacing)
  {
    // Load YUYV fields
    LoadPlane(buf.fields[FIELD_TOP][0], GL_BGRA,
              im->width / 2, im->height >> 1,
              im->stride[0] * 2, im->bpp, im->plane[0]);

    LoadPlane(buf.fields[FIELD_BOT][0], GL_BGRA,
              im->width / 2, im->height >> 1,
              im->stride[0] * 2, im->bpp, im->plane[0] + im->stride[0]);
  }
  else
  {
    // Load YUYV plane
    LoadPlane(buf.fields[FIELD_FULL][0], GL_BGRA,
              im->width / 2, im->height,
              im->stride[0], im->bpp, im->plane[0]);
  }

  VerifyGLState();

  CalculateTextureSourceRects(source, 3);

  return true;
}

void CLinuxRendererGL::DeleteYUV422PackedTexture(int index)
{
  YUVBUFFER& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

  if (buf.fields[FIELD_FULL][0].id == 0)
    return;

  // finish up all textures, and delete them
  for (int f = 0;f<MAX_FIELDS;f++)
  {
    if (buf.fields[f][0].id)
    {
      if (glIsTexture(buf.fields[f][0].id))
      {
        glDeleteTextures(1, &buf.fields[f][0].id);
      }
      buf.fields[f][0].id = 0;
    }
    buf.fields[f][1].id = 0;
    buf.fields[f][2].id = 0;
  }

  if (pbo[0])
  {
    if (im.plane[0])
    {
      glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[0]);
      glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
      im.plane[0] = NULL;
      glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
    }
    glDeleteBuffersARB(1, pbo);
    pbo[0] = 0;
  }
  else
  {
    if (im.plane[0])
    {
      delete[] im.plane[0];
      im.plane[0] = NULL;
    }
  }
}

bool CLinuxRendererGL::CreateYUV422PackedTexture(int index)
{
  // since we also want the field textures, pitch must be texture aligned
  YUVBUFFER& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

  // Delete any old texture
  DeleteYUV422PackedTexture(index);

  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;
  im.cshift_x = 0;
  im.cshift_y = 0;
  im.bpp = 1;

  im.stride[0] = im.width * 2;
  im.stride[1] = 0;
  im.stride[2] = 0;

  im.plane[0] = NULL;
  im.plane[1] = NULL;
  im.plane[2] = NULL;

  // packed YUYV plane
  im.planesize[0] = im.stride[0] * im.height;
  // second plane is not used
  im.planesize[1] = 0;
  // third plane is not used
  im.planesize[2] = 0;

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffersARB(1, pbo);

    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[0]);
    glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, im.planesize[0] + PBO_OFFSET, 0, GL_STREAM_DRAW_ARB);
    void* pboPtr = glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
    if (pboPtr)
    {
      im.plane[0] = (uint8_t*)pboPtr + PBO_OFFSET;
      memset(im.plane[0], 0, im.planesize[0]);
    }
    else
    {
      CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
      pboSetup = false;
    }

    if (!pboSetup)
    {
      glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, *pbo);
      glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
      glDeleteBuffersARB(1, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
  }

  if (!pboSetup)
  {
    im.plane[0] = new uint8_t[im.planesize[0]];
  }

  for(int f = 0;f<MAX_FIELDS;f++)
  {
    if (!glIsTexture(buf.fields[f][0].id))
    {
      glGenTextures(1, &buf.fields[f][0].id);
      VerifyGLState();
    }
    buf.fields[f][0].pbo = pbo[0];
    buf.fields[f][1].id = buf.fields[f][0].id;
    buf.fields[f][2].id = buf.fields[f][1].id;
  }

  // YUV
  for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
  {
    int fieldshift = (f==FIELD_FULL) ? 0 : 1;
    YUVPLANE (&planes)[YuvImage::MAX_PLANES] = buf.fields[f];

    planes[0].texwidth  = im.width / 2;
    planes[0].texheight = im.height >> fieldshift;
    planes[1].texwidth  = planes[0].texwidth;
    planes[1].texheight = planes[0].texheight;
    planes[2].texwidth  = planes[1].texwidth;
    planes[2].texheight = planes[1].texheight;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 2;
      planes[p].pixpertex_y = 1;
    }

    YUVPLANE &plane = planes[0];
    if (plane.texwidth * plane.texheight == 0)
      continue;

    glBindTexture(m_textureTarget, plane.id);

    glTexImage2D(m_textureTarget, 0, GL_RGBA, plane.texwidth, plane.texheight, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    VerifyGLState();
  }

  return true;
}

void CLinuxRendererGL::SetTextureFilter(GLenum method)
{
  for (int i = 0 ; i<m_NumYV12Buffers ; i++)
  {
    YUVBUFFER& buf = m_buffers[i];

    for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
    {
      for (int p = 0; p < 3; p++)
      {
        if(glIsTexture(buf.fields[f][p].id))
        {
          glBindTexture(m_textureTarget, buf.fields[f][p].id);
          glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, method);
          glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, method);
          VerifyGLState();
        }
      }
    }
  }
}

bool CLinuxRendererGL::Supports(ERENDERFEATURE feature)
{
  if(feature == RENDERFEATURE_BRIGHTNESS)
  {
    return (m_renderMethod & RENDER_GLSL);
  }
  
  if(feature == RENDERFEATURE_CONTRAST)
  {
    return (m_renderMethod & RENDER_GLSL);
  }

  if(feature == RENDERFEATURE_GAMMA)
    return false;
  
  if(feature == RENDERFEATURE_NOISE)
    return false;

  if(feature == RENDERFEATURE_SHARPNESS)
  {
    return false;
  }

  if (feature == RENDERFEATURE_NONLINSTRETCH)
  {
    if (m_renderMethod & RENDER_GLSL)
      return true;
  }

  if (feature == RENDERFEATURE_STRETCH         ||
      feature == RENDERFEATURE_ZOOM            ||
      feature == RENDERFEATURE_VERTICAL_SHIFT  ||
      feature == RENDERFEATURE_PIXEL_RATIO     ||
      feature == RENDERFEATURE_POSTPROCESS     ||
      feature == RENDERFEATURE_ROTATION)
    return true;

  return false;
}

bool CLinuxRendererGL::SupportsMultiPassRendering()
{
  return g_Windowing.IsExtSupported("GL_EXT_framebuffer_object");
}

bool CLinuxRendererGL::Supports(ESCALINGMETHOD method)
{
  //nearest neighbor doesn't work on YUY2 and UYVY
  if (method == VS_SCALINGMETHOD_NEAREST &&
      m_format != AV_PIX_FMT_YUYV422 &&
      m_format != AV_PIX_FMT_UYVY422)
    return true;

  if (method == VS_SCALINGMETHOD_LINEAR ||
      method == VS_SCALINGMETHOD_AUTO)
    return true;

  if (method == VS_SCALINGMETHOD_CUBIC ||
      method == VS_SCALINGMETHOD_LANCZOS2 ||
      method == VS_SCALINGMETHOD_SPLINE36_FAST ||
      method == VS_SCALINGMETHOD_LANCZOS3_FAST ||
      method == VS_SCALINGMETHOD_SPLINE36 ||
      method == VS_SCALINGMETHOD_LANCZOS3)
  {
    // if scaling is below level, avoid hq scaling
    float scaleX = fabs(((float)m_sourceWidth - m_destRect.Width())/m_sourceWidth)*100;
    float scaleY = fabs(((float)m_sourceHeight - m_destRect.Height())/m_sourceHeight)*100;
    int minScale = CServiceBroker::GetSettings().GetInt(CSettings::SETTING_VIDEOPLAYER_HQSCALERS);
    if (scaleX < minScale && scaleY < minScale)
      return false;

    bool hasFramebuffer = false;
    unsigned int major, minor;
    g_Windowing.GetRenderVersion(major, minor);
    if (major > 3 ||
        (major == 3 && minor >= 2))
      hasFramebuffer = true;
    if (g_Windowing.IsExtSupported("GL_EXT_framebuffer_object"))
      hasFramebuffer = true;
    if (hasFramebuffer  && (m_renderMethod & RENDER_GLSL))
    {
      // spline36 and lanczos3 are only allowed through advancedsettings.xml
      if(method != VS_SCALINGMETHOD_SPLINE36
      && method != VS_SCALINGMETHOD_LANCZOS3)
        return true;
      else
        return g_advancedSettings.m_videoEnableHighQualityHwScalers;
    }
  }
 
  return false;
}

void CLinuxRendererGL::BindPbo(YUVBUFFER& buff)
{
  bool pbo = false;
  for(int plane = 0; plane < YuvImage::MAX_PLANES; plane++)
  {
    if(!buff.pbo[plane] || buff.image.plane[plane] == (uint8_t*)PBO_OFFSET)
      continue;
    pbo = true;

    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, buff.pbo[plane]);
    glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
    buff.image.plane[plane] = (uint8_t*)PBO_OFFSET;
  }
  if (pbo)
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
}

void CLinuxRendererGL::UnBindPbo(YUVBUFFER& buff)
{
  bool pbo = false;
  for(int plane = 0; plane < YuvImage::MAX_PLANES; plane++)
  {
    if(!buff.pbo[plane] || buff.image.plane[plane] != (uint8_t*)PBO_OFFSET)
      continue;
    pbo = true;

    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, buff.pbo[plane]);
    glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, buff.image.planesize[plane] + PBO_OFFSET, NULL, GL_STREAM_DRAW_ARB);
    buff.image.plane[plane] = (uint8_t*)glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB) + PBO_OFFSET;
  }
  if (pbo)
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
}

CRenderInfo CLinuxRendererGL::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = NUM_BUFFERS;
  return info;
}

// Color management helpers

bool CLinuxRendererGL::LoadCLUT()
{
  DeleteCLUT();

  int clutSize, dataSize;
  if (!CColorManager::Get3dLutSize(CMS_DATA_FMT_RGB, &clutSize, &dataSize))
    return 0;

  // allocate buffer
  m_CLUTsize = clutSize;
  m_CLUT = static_cast<uint16_t*>(malloc(dataSize));

  // load 3DLUT
  if ( !m_ColorManager->GetVideo3dLut(m_iFlags, &m_cmsToken, CMS_DATA_FMT_RGB, m_CLUTsize, m_CLUT) )
  {
    free(m_CLUT);
    CLog::Log(LOGERROR, "Error loading the LUT");
    return false;
  }

  // create 3DLUT texture
  CLog::Log(LOGDEBUG, "LinuxRendererGL: creating 3DLUT");
  glGenTextures(1, &m_tCLUTTex);
  glActiveTexture(GL_TEXTURE4);
  if ( m_tCLUTTex <= 0 )
  {
    CLog::Log(LOGERROR, "Error creating 3DLUT texture");
    return false;
  }

  // bind and set 3DLUT texture parameters
  glBindTexture(GL_TEXTURE_3D, m_tCLUTTex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

  // load 3DLUT data
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16, m_CLUTsize, m_CLUTsize, m_CLUTsize, 0, GL_RGB16, GL_UNSIGNED_SHORT, m_CLUT);
  free(m_CLUT);
  glActiveTexture(GL_TEXTURE0);
  return true;
}

void CLinuxRendererGL::DeleteCLUT()
{
  if (m_tCLUTTex)
  {
    CLog::Log(LOGDEBUG, "LinuxRendererGL: deleting 3DLUT");
    glDeleteTextures(1, &m_tCLUTTex);
    m_tCLUTTex = 0;
  }
}

