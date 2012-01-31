#include <algorithm>
#include <fstream>
#include <iostream>
#include <cmath>
#include <vector>
#include <new>
#include <exception>
using namespace std;

#include "GL/glew.h"
#include "GL/freeglut.h"
#include "AntTweakBar.h"

#include "glu.hpp"

#include "vec4.h"
#include "mat4.h"
#include "Program.h"

#ifdef _WIN32
#include <windows.h>
#include <time.h>
#else
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#include "BMPLoader.h"

#define BUFFER_OFFSET(i) 	((char *)NULL + (i))

double bencher = 0.0;
double frames = 0.0;

namespace
{
// Window Variables
namespace window
{
int width   = 1600;
int height  = 900;
} // namespace window

// GL Variables
namespace gl
{
// RB0 detail
namespace rbuffer
{
enum
{
	DEPTH_FBO_JACOBIANS,
	MAX
};
}

// FBO detail
namespace fbuffer
{
enum
{
    FFT0 = 0,
    FFT1,
    SKY,
    VARIANCES,
    FOAM,
    GAUSS,
    TILE,

    MAX
};
} // namespace fbuffer

namespace texture
{
enum
{
    IRRADIANCE = 0,
    INSCATTER,
    TRANSMITTANCE,
    SKY,
    NOISE,
    SPECTRUM12,
    SPECTRUM34,
    SLOPE_VARIANCE,
    FFT_PING,
    FFT_PONG,
    BUTTERFLY,
    FOAM_SURFACE,
    FOAM,
    GAUSSZ,
    FOAM_NORMALMAP,

    MAX
};
}   // namespace texture

namespace buffer
{
enum
{
    INDEX_GRID = 0,
    VERTEX_GRID,

    MAX
};
}   // namespace buffer


namespace program
{
enum
{
    RENDER = 0,
    SKY,
    SKYMAP,
    CLOUDS,
    SHOW_SPECTRUM,
    INIT,
    INIT2,
    VARIANCES,
    FFTX,
    FFTY,
    JACOBIANS,
    FOAM,
    GAUSS,
//    PARTICLE_GEN,
//    PARTICLE_UPDATE,
//    WHITECAP_COVERAGE,
//    WHITECAP_PARTICLE,
//    WHITECAP_UPDATE,
//    TILE,

    MAX
};
} // namespace program

// GL objects
GLuint      rbuffers[rbuffer::MAX];
GLuint      fbuffers[fbuffer::MAX];
GLuint      textures[texture::MAX];
GLuint      buffers[buffer::MAX];
Program*    programs[program::MAX];

} // namespace gl

// TW
namespace tw
{
	TwBar* bar = NULL;
}

// camera
namespace camera
{
float z 	    = 20.0f;
float velx		= 0.0f;
float vely		= 0.0f;
float velz		= 0.0f;
float x			= 0.0f;
float y			= 0.0f;
float theta 	= 0.0f;
float phi 		= 0.0f;
float fovy 		= 90.0f;
float vel		= 5.0f;
}

// Various
unsigned int skyTexSize = 512;
bool cloudLayer = false;
float octaves = 10.0;
float lacunarity = 2.2;
float gain = 0.7;
float norm = 0.5;
float clamp1 = -0.15;
float clamp2 = 0.2;
float cloudColor[4] = { 1.0, 1.0, 1.0, 1.0 };
vec4f vboParams;
int vboSize = 0;
int vboVertices = 0;
float sunTheta = 0.6*M_PI / 2.0 - 0.05;
float sunPhi = 0.0;
float gridSize = 4.0f;

// render ing options
float seaColor[4] = {8.0 / 255.0, 124.0 / 255.0, 152.0 / 255.0, 0.15};
float hdrExposure = 0.4;
bool grid = false;
bool animate = true;
bool seaContrib = true;
bool sunContrib = false;
bool skyContrib = true;
bool foamContrib = true;
bool manualFilter = false;
bool show_spectrum = false;
float show_spectrum_zoom = 1.0;
bool show_spectrum_linear = false;
bool normals = false;
bool choppy = true;
float choppy_factor0 = 2.4f;	// Control Choppiness
float choppy_factor1 = 2.4f;	// Control Choppiness
float choppy_factor2 = 2.4f;	// Control Choppiness
float choppy_factor3 = 2.4f;	// Control Choppiness

// WAVES SPECTRUM
const int N_SLOPE_VARIANCE = 10; // size of the 3d texture containing precomputed filtered slope variances
float GRID1_SIZE = 5409.0; // size in meters (i.e. in spatial domain) of the first grid
float GRID2_SIZE = 503.0; // size in meters (i.e. in spatial domain) of the second grid
float GRID3_SIZE = 51.0; // size in meters (i.e. in spatial domain) of the third grid
float GRID4_SIZE = 5.0; // size in meters (i.e. in spatial domain) of the fourth grid
float WIND = 10.0; // wind speed in meters per second (at 10m above surface)
float OMEGA = 0.84f; // sea state (inverse wave age)
bool propagate = true; // wave propagation?
float A = 1.0; // wave amplitude factor (should be one)
const float cm = 0.23; // Eq 59
const float km = 370.0; // Eq 59
float speed = 1.0f;

// FFT WAVES
const int PASSES = 8; // number of passes needed for the FFT 6 -> 64, 7 -> 128, 8 -> 256, etc
const int FFT_SIZE = 1 << PASSES; // size of the textures storing the waves in frequency and spatial domains
float *spectrum12 = NULL;
float *spectrum34 = NULL;
int fftPass = 0;

// Foam
float jacobian_scale = -0.1f;//1.20f;

} // namespace


float sqr(float x)
{
    return x * x;
}

float omega(float k)
{
    return sqrt(9.81 * k * (1.0 + sqr(k / km))); // Eq 24
}

// 1/kx and 1/ky in meters
float spectrum(float kx, float ky, bool omnispectrum = false)
{
    float U10 = WIND;
    float Omega = OMEGA;

    // phase speed
    float k = sqrt(kx * kx + ky * ky);
    float c = omega(k) / k;

    // spectral peak
    float kp = 9.81 * sqr(Omega / U10); // after Eq 3
    float cp = omega(kp) / kp;

    // friction velocity
    float z0 = 3.7e-5 * sqr(U10) / 9.81 * pow(U10 / cp, 0.9f); // Eq 66
    float u_star = 0.41 * U10 / log(10.0 / z0); // Eq 60

    float Lpm = exp(- 5.0 / 4.0 * sqr(kp / k)); // after Eq 3
    float gamma = Omega < 1.0 ? 1.7 : 1.7 + 6.0 * log(Omega); // after Eq 3 // log10 or log??
    float sigma = 0.08 * (1.0 + 4.0 / pow(Omega, 3.0f)); // after Eq 3
    float Gamma = exp(-1.0 / (2.0 * sqr(sigma)) * sqr(sqrt(k / kp) - 1.0));
    float Jp = pow(gamma, Gamma); // Eq 3
    float Fp = Lpm * Jp * exp(- Omega / sqrt(10.0) * (sqrt(k / kp) - 1.0)); // Eq 32
    float alphap = 0.006 * sqrt(Omega); // Eq 34
    float Bl = 0.5 * alphap * cp / c * Fp; // Eq 31

    float alpham = 0.01 * (u_star < cm ? 1.0 + log(u_star / cm) : 1.0 + 3.0 * log(u_star / cm)); // Eq 44
    float Fm = exp(-0.25 * sqr(k / km - 1.0)); // Eq 41
    float Bh = 0.5 * alpham * cm / c * Fm; // Eq 40

    Bh *= Lpm; // bug fix???

    if (omnispectrum)
    {
        return A * (Bl + Bh) / (k * sqr(k)); // Eq 30
    }

    float a0 = log(2.0) / 4.0;
    float ap = 4.0;
    float am = 0.13 * u_star / cm; // Eq 59
    float Delta = tanh(a0 + ap * pow(c / cp, 2.5f) + am * pow(cm / c, 2.5f)); // Eq 57

    float phi = atan2(ky, kx);

    if (propagate)
    {
        if (kx < 0.0)
        {
            return 0.0;
        }
        else
        {
            Bl *= 2.0;
            Bh *= 2.0;
        }
    }

	// remove waves perpendicular to wind dir
	float tweak = sqrt(std::max(kx/sqrt(kx*kx+ky*ky),0.0f));
	tweak = 1.0f;
    return A * (Bl + Bh) * (1.0 + Delta * cos(2.0 * phi)) / (2.0 * M_PI * sqr(sqr(k))) * tweak; // Eq 67
}

void drawQuad()
{
    glBegin(GL_TRIANGLE_STRIP);
    glVertex4f(-1.0, -1.0, 0.0, 0.0);
    glVertex4f(+1.0, -1.0, 1.0, 0.0);
    glVertex4f(-1.0, +1.0, 0.0, 1.0);
    glVertex4f(+1.0, +1.0, 1.0, 1.0);
    glEnd();
}

// ----------------------------------------------------------------------------
// CLOUDS
// ----------------------------------------------------------------------------

void drawClouds(const vec4f &sun, const mat4f &mat)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(gl::programs[gl::program::CLOUDS]->program);
    glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "worldToScreen"), 1, true, mat.coefficients());
    glUniform3f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "worldCamera"), 0.0, 0.0, camera::z);
    glUniform3f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "worldSunDir"), sun.x, sun.y, sun.z);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "hdrExposure"), hdrExposure);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "octaves"), octaves);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "lacunarity"), lacunarity);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "gain"), gain);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "norm"), norm);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "clamp1"), clamp1);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "clamp2"), clamp2);
    glUniform4f(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "cloudsColor"), cloudColor[0], cloudColor[1], cloudColor[2], cloudColor[3]);
    glBegin(GL_TRIANGLE_STRIP);
    glVertex3f(-1e6, -1e6, 3000.0);
    glVertex3f(1e6, -1e6, 3000.0);
    glVertex3f(-1e6, 1e6, 3000.0);
    glVertex3f(1e6, 1e6, 3000.0);
    glEnd();
    glDisable(GL_BLEND);
}

// ----------------------------------------------------------------------------
// PROGRAM RELOAD
// ----------------------------------------------------------------------------

void loadPrograms(bool all)
{
	char* files[2];
	char options[512];
	files[0] = "atmosphere.glsl";
	files[1] = "ocean.glsl";
	sprintf(options, "#define %sSEA_CONTRIB\n#define %sSUN_CONTRIB\n#define %sSKY_CONTRIB\n#define %sCLOUDS\n#define %sHARDWARE_ANISTROPIC_FILTERING\n#define %sFOAM_CONTRIB\n",
		    seaContrib ? "" : "NO_", sunContrib ? "" : "NO_", skyContrib ? "" : "NO_", cloudLayer ? "" : "NO_", manualFilter ? "NO_" : "", foamContrib ? "" : "NO_");

	if (gl::programs[gl::program::RENDER] != NULL)
	{
		delete gl::programs[gl::program::RENDER];
		gl::programs[gl::program::RENDER] = NULL;
	}
	gl::programs[gl::program::RENDER] = new Program(2, files, options);
	glUseProgram(gl::programs[gl::program::RENDER]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "skyIrradianceSampler"), gl::texture::IRRADIANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "inscatterSampler"), gl::texture::INSCATTER);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "transmittanceSampler"), gl::texture::TRANSMITTANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "skySampler"), gl::texture::SKY);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "spectrum_1_2_Sampler"), gl::texture::SPECTRUM12);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "spectrum_3_4_Sampler"), gl::texture::SPECTRUM34);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "slopeVarianceSampler"), gl::texture::SLOPE_VARIANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "noiseSampler2"), gl::texture::FOAM_SURFACE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "foamNormalSampler"), gl::texture::FOAM_NORMALMAP);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "foamSampler"), gl::texture::FOAM);

	files[0] = "foam.glsl";
	if (gl::programs[gl::program::FOAM] != NULL)
	{
		delete gl::programs[gl::program::FOAM];
		gl::programs[gl::program::FOAM] = NULL;
	}
	gl::programs[gl::program::FOAM] = new Program(1, files);
	glUseProgram(gl::programs[gl::program::FOAM]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "gausszSampler"), gl::texture::GAUSSZ);

	if (!all)
	{
		return;
	}

	files[0] = "atmosphere.glsl";
	files[1] = "sky.glsl";
	if (gl::programs[gl::program::SKY] != NULL)
	{
		delete gl::programs[gl::program::SKY];
		gl::programs[gl::program::SKY] = NULL;
	}
	gl::programs[gl::program::SKY] = new Program(2, files, options);
	glUseProgram(gl::programs[gl::program::SKY]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "gl::programs[gl::program::SKY]IrradianceSampler"), gl::texture::IRRADIANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "inscatterSampler"), gl::texture::INSCATTER);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "transmittanceSampler"), gl::texture::TRANSMITTANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "skySampler"), gl::texture::SKY);

	files[0] = "atmosphere.glsl";
	files[1] = "skymap.glsl";
	if (gl::programs[gl::program::SKYMAP] != NULL)
	{
		delete gl::programs[gl::program::SKYMAP];
		gl::programs[gl::program::SKYMAP] = NULL;
	}
	gl::programs[gl::program::SKYMAP] = new Program(2, files, options);
	glUseProgram(gl::programs[gl::program::SKYMAP]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "skyIrradianceSampler"), gl::texture::IRRADIANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "inscatterSampler"), gl::texture::INSCATTER);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "transmittanceSampler"), gl::texture::TRANSMITTANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "noiseSampler"), gl::texture::NOISE);

	if (gl::programs[gl::program::CLOUDS] == NULL)
	{
		files[0] = "atmosphere.glsl";
		files[1] = "clouds.glsl";
		gl::programs[gl::program::CLOUDS] = new Program(2, files);
		glUseProgram(gl::programs[gl::program::CLOUDS]->program);
		glUniform1i(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "skyIrradianceSampler"), gl::texture::IRRADIANCE);
		glUniform1i(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "inscatterSampler"), gl::texture::INSCATTER);
		glUniform1i(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "transmittanceSampler"), gl::texture::TRANSMITTANCE);
		glUniform1i(glGetUniformLocation(gl::programs[gl::program::CLOUDS]->program, "noiseSampler"), gl::texture::NOISE);
	}

	files[0] = "spectrum.glsl";
	if (gl::programs[gl::program::SHOW_SPECTRUM] != NULL)
	{
		delete gl::programs[gl::program::SHOW_SPECTRUM];
		gl::programs[gl::program::SHOW_SPECTRUM] = NULL;
	}
	gl::programs[gl::program::SHOW_SPECTRUM] = new Program(1, files);
	glUseProgram(gl::programs[gl::program::SHOW_SPECTRUM]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SHOW_SPECTRUM]->program, "spectrum_1_2_Sampler"), gl::texture::SPECTRUM12);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::SHOW_SPECTRUM]->program, "spectrum_3_4_Sampler"), gl::texture::SPECTRUM34);

	files[0] = "init.glsl";
	if (gl::programs[gl::program::INIT] != NULL)
	{
		delete gl::programs[gl::program::INIT];
		gl::programs[gl::program::INIT] = NULL;
	}
	gl::programs[gl::program::INIT] = new Program(1, files);
	glUseProgram(gl::programs[gl::program::INIT]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::INIT]->program, "spectrum_1_2_Sampler"), gl::texture::SPECTRUM12);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::INIT]->program, "spectrum_3_4_Sampler"), gl::texture::SPECTRUM34);

	files[0] = "init2.glsl";
	if (gl::programs[gl::program::INIT2] != NULL)
	{
		delete gl::programs[gl::program::INIT2];
		gl::programs[gl::program::INIT2] = NULL;
	}
	gl::programs[gl::program::INIT2] = new Program(1, files);
	glUseProgram(gl::programs[gl::program::INIT2]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::INIT2]->program, "spectrum_1_2_Sampler"), gl::texture::SPECTRUM12);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::INIT2]->program, "spectrum_3_4_Sampler"), gl::texture::SPECTRUM34);


	files[0] = "variances.glsl";
	if (gl::programs[gl::program::VARIANCES] != NULL)
	{
		delete gl::programs[gl::program::VARIANCES];
		gl::programs[gl::program::VARIANCES] = NULL;
	}
	gl::programs[gl::program::VARIANCES] = new Program(1, files);
	glUseProgram(gl::programs[gl::program::VARIANCES]->program);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::VARIANCES]->program, "N_SLOPE_VARIANCE"), N_SLOPE_VARIANCE);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::VARIANCES]->program, "spectrum_1_2_Sampler"), gl::texture::SPECTRUM12);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::VARIANCES]->program, "spectrum_3_4_Sampler"), gl::texture::SPECTRUM34);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::VARIANCES]->program, "FFT_SIZE"), FFT_SIZE);

	files[0] = "fftx.glsl";
	if (gl::programs[gl::program::FFTX] != NULL)
	{
		delete gl::programs[gl::program::FFTX];
		gl::programs[gl::program::FFTX] = NULL;
	}
	gl::programs[gl::program::FFTX] = new Program(1, files);
	glProgramParameteriEXT(gl::programs[gl::program::FFTX]->program, GL_GEOMETRY_VERTICES_OUT_EXT, 24);
	glLinkProgram(gl::programs[gl::program::FFTX]->program);
	glUseProgram(gl::programs[gl::program::FFTX]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "butterflySampler"), gl::texture::BUTTERFLY);

	files[0] = "ffty.glsl";
	if (gl::programs[gl::program::FFTY] != NULL)
	{
		delete gl::programs[gl::program::FFTY];
		gl::programs[gl::program::FFTY] = NULL;
	}
	gl::programs[gl::program::FFTY] = new Program(1, files);
	glProgramParameteriEXT(gl::programs[gl::program::FFTY]->program, GL_GEOMETRY_VERTICES_OUT_EXT, 24);
	glLinkProgram(gl::programs[gl::program::FFTY]->program);
	glUseProgram(gl::programs[gl::program::FFTY]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "butterflySampler"), gl::texture::BUTTERFLY);

	files[0] = "gaussz.glsl";
	if (gl::programs[gl::program::GAUSS] != NULL)
	{
		delete gl::programs[gl::program::GAUSS];
		gl::programs[gl::program::GAUSS] = NULL;
	}
	gl::programs[gl::program::GAUSS] = new Program(1, files);

	// Back to default pipeline
	glUseProgram(0);
}

void TW_CALL getBool(void *value, void *clientData)
{
    *((bool*) value) = *((bool*) clientData);
}

void TW_CALL setBool(const void *value, void *clientData)
{
    *((bool*) clientData) = *((bool*) value);
    loadPrograms(clientData == &cloudLayer);
}


// ----------------------------------------------------------------------------
// MESH GENERATION
// ----------------------------------------------------------------------------

float frandom(long *seed);

void generateMesh()
{
    if (vboSize != 0)
    {
        glDeleteBuffers(1, &gl::buffers[gl::buffer::VERTEX_GRID]);
        glDeleteBuffers(1, &gl::buffers[gl::buffer::INDEX_GRID]);
    }
    glGenBuffers(1, &gl::buffers[gl::buffer::VERTEX_GRID]); // Hope there's a good memory manager ...
    glBindBuffer(GL_ARRAY_BUFFER, gl::buffers[gl::buffer::VERTEX_GRID]);

    float horizon = tan(camera::theta / 180.0 * M_PI);
    float s = min(1.1f, 0.5f + horizon * 0.5f);

    float vmargin = 0.1;
    float hmargin = 0.1;

//    vboParams = vec4f(window::width, window::height, gridSize, camera::theta);
    vec4f *data = new vec4f[int(ceil(window::height * (s + vmargin) / gridSize) + 5) * int(ceil(window::width * (1.0 + 2.0 * hmargin) / gridSize) + 5)];

//    long seed = 1234;

    int n = 0;
    int nx = 0;
    for (float j = window::height * s - 0.1/* - gridSize*/; j > -window::height * vmargin - gridSize; j -= gridSize)
    {
        nx = 0;
        for (float i = -window::width * hmargin; i < window::width * (1.0 + hmargin) + gridSize; i += gridSize)
        {
            data[n++] = vec4f(-1.0 + 2.0 * i / window::width, -1.0 + 2.0 * j / window::height, 0.0, 1.0);
            //data[n++] = vec4f(-1.0 + 2.0 * (i + (frandom(&seed)-0.5)*gridSize*0.25) / width, -1.0 + 2.0 * (j + (frandom(&seed)-0.5)*gridSize*0.25) / height, 0.0, 1.0);
            nx++;
//            std::cout << data[n-1].x << " " << data[n-1].y << std::endl;
        }
    }
//    std::cout << "generating\n" << std::endl;
	vboVertices = n;
    glBufferData(GL_ARRAY_BUFFER, n * 16, data, GL_STATIC_DRAW);
    delete[] data;

    glGenBuffers(1, &gl::buffers[gl::buffer::INDEX_GRID]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl::buffers[gl::buffer::INDEX_GRID]);

    vboSize = 0;
    GLuint *indices = new GLuint[6 * int(ceil(window::height * (s + vmargin) / gridSize) + 4) * int(ceil(window::width * (1.0 + 2.0 * hmargin) / gridSize) + 4)];

    int nj = 0;
    for (float j = window::height * s - 0.1; j > -window::height * vmargin; j -= gridSize)
    {
        int ni = 0;
        for (float i = -window::width * hmargin; i < window::width * (1.0 + hmargin); i += gridSize)
        {
            indices[vboSize++] = ni + (nj + 1) * nx;
            indices[vboSize++] = (ni + 1) + (nj + 1) * nx;
            indices[vboSize++] = (ni + 1) + nj * nx;

            indices[vboSize++] = ni + nj * nx;
            indices[vboSize++] = ni + (nj + 1) * nx;
            indices[vboSize++] = (ni + 1) + nj * nx;
            ni++;
        }
        nj++;
    }
	// Should prefer uint16 to uint32
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, vboSize * 4, indices, GL_STATIC_DRAW);
    delete[] indices;

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// ----------------------------------------------------------------------------
// WAVES SPECTRUM GENERATION
// ----------------------------------------------------------------------------

long lrandom(long *seed)
{
    *seed = (*seed * 1103515245 + 12345) & 0x7FFFFFFF;
    return *seed;
}

float frandom(long *seed)
{
    long r = lrandom(seed) >> (31 - 24);
    return r / (float)(1 << 24);
}

inline float grandom(float mean, float stdDeviation, long *seed)
{
    float x1, x2, w, y1;
    static float y2;
    static int use_last = 0;

    if (use_last)
    {
        y1 = y2;
        use_last = 0;
    }
    else
    {
        do
        {
            x1 = 2.0f * frandom(seed) - 1.0f;
            x2 = 2.0f * frandom(seed) - 1.0f;
            w  = x1 * x1 + x2 * x2;
        }
        while (w >= 1.0f);
        w  = sqrt((-2.0f * log(w)) / w);
        y1 = x1 * w;
        y2 = x2 * w;
        use_last = 1;
    }
    return mean + y1 * stdDeviation;
}

void getSpectrumSample(int i, int j, float lengthScale, float kMin, float *result)
{
    static long seed = 1234;
    float dk = 2.0 * M_PI / lengthScale;
    float kx = i * dk;
    float ky = j * dk;
    if (abs(kx) < kMin && abs(ky) < kMin)
    {
        result[0] = 0.0;
        result[1] = 0.0;
    }
    else
    {
        float S = spectrum(kx, ky);
        float h = sqrt(S / 2.0) * dk;
        float phi = frandom(&seed) * 2.0 * M_PI;
        result[0] = h * cos(phi);
        result[1] = h * sin(phi);
    }
}

// generates the waves spectrum
void generateWavesSpectrum()
{
    if (spectrum12 != NULL)
    {
        delete[] spectrum12;
        delete[] spectrum34;
    }
    spectrum12 = new float[FFT_SIZE * FFT_SIZE * 4];
    spectrum34 = new float[FFT_SIZE * FFT_SIZE * 4];

    for (int y = 0; y < FFT_SIZE; ++y)
    {
        for (int x = 0; x < FFT_SIZE; ++x)
        {
            int offset = 4 * (x + y * FFT_SIZE);
            int i = x >= FFT_SIZE / 2 ? x - FFT_SIZE : x;
            int j = y >= FFT_SIZE / 2 ? y - FFT_SIZE : y;
            getSpectrumSample(i, j, GRID1_SIZE, M_PI / GRID1_SIZE, spectrum12 + offset);
            getSpectrumSample(i, j, GRID2_SIZE, M_PI * FFT_SIZE / GRID1_SIZE, spectrum12 + offset + 2);
            getSpectrumSample(i, j, GRID3_SIZE, M_PI * FFT_SIZE / GRID2_SIZE, spectrum34 + offset);
            getSpectrumSample(i, j, GRID4_SIZE, M_PI * FFT_SIZE / GRID3_SIZE, spectrum34 + offset + 2);
        }
    }

    glActiveTexture(GL_TEXTURE0 + gl::texture::SPECTRUM12);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, FFT_SIZE, FFT_SIZE, 0, GL_RGBA, GL_FLOAT, spectrum12);
    glActiveTexture(GL_TEXTURE0 + gl::texture::SPECTRUM34);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, FFT_SIZE, FFT_SIZE, 0, GL_RGBA, GL_FLOAT, spectrum34);
    TwDefine("Parameters color='255 0 0'");
}

float getSlopeVariance(float kx, float ky, float *spectrumSample)
{
    float kSquare = kx * kx + ky * ky;
    float real = spectrumSample[0];
    float img = spectrumSample[1];
    float hSquare = real * real + img * img;
    return kSquare * hSquare * 2.0;
}

// precomputes filtered slope variances in a 3d texture, based on the wave spectrum
// (weird behaviour when amplitudes grow : water looks like plastic)
void TW_CALL computeSlopeVarianceTex(void *unused)
{
    // slope variance due to all waves, by integrating over the full spectrum
    float theoreticSlopeVariance = 0.0;
    float k = 5e-3;
    while (k < 1e3)
    {
        float nextK = k * 1.001;
        theoreticSlopeVariance += k * k * spectrum(k, 0, true) * (nextK - k);
        k = nextK;
    }

    // slope variance due to waves, by integrating over the spectrum part
    // that is covered by the four nested grids. This can give a smaller result
    // than the theoretic total slope variance, because the higher frequencies
    // may not be covered by the four nested grid. Hence the difference between
    // the two is added as a "delta" slope variance in the "variances" shader,
    // to be sure not to lose the variance due to missing wave frequencies in
    // the four nested grids
    float totalSlopeVariance = 0.0;
    for (int y = 0; y < FFT_SIZE; ++y)
    {
        for (int x = 0; x < FFT_SIZE; ++x)
        {
            int offset = 4 * (x + y * FFT_SIZE);
            float i = 2.0 * M_PI * (x >= FFT_SIZE / 2 ? x - FFT_SIZE : x);
            float j = 2.0 * M_PI * (y >= FFT_SIZE / 2 ? y - FFT_SIZE : y);
            totalSlopeVariance += getSlopeVariance(i / GRID1_SIZE, j / GRID1_SIZE, spectrum12 + offset);
            totalSlopeVariance += getSlopeVariance(i / GRID2_SIZE, j / GRID2_SIZE, spectrum12 + offset + 2);
            totalSlopeVariance += getSlopeVariance(i / GRID3_SIZE, j / GRID3_SIZE, spectrum34 + offset);
            totalSlopeVariance += getSlopeVariance(i / GRID4_SIZE, j / GRID4_SIZE, spectrum34 + offset + 2);
        }
    }

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::VARIANCES]);
    glViewport(0, 0, N_SLOPE_VARIANCE, N_SLOPE_VARIANCE);

    glUseProgram(gl::programs[gl::program::VARIANCES]->program);
    glUniform4f(glGetUniformLocation(gl::programs[gl::program::VARIANCES]->program, "GRID_SIZES"), GRID1_SIZE, GRID2_SIZE, GRID3_SIZE, GRID4_SIZE);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::VARIANCES]->program, "slopeVarianceDelta"), theoreticSlopeVariance - totalSlopeVariance);

    for (int layer = 0; layer < N_SLOPE_VARIANCE; ++layer)
    {
        glFramebufferTexture3DEXT(	GL_FRAMEBUFFER_EXT,
									GL_COLOR_ATTACHMENT0_EXT,
									GL_TEXTURE_3D,
									gl::textures[gl::texture::SLOPE_VARIANCE],
									0,
									layer	);
        glUniform1f(glGetUniformLocation(gl::programs[gl::program::VARIANCES]->program, "c"), layer);
        drawQuad();
    }

    TwDefine("Parameters color='17 109 143'");
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
}


// ----------------------------------------------------------------------------
// WAVES GENERATION AND ANIMATION (using FFT on GPU)
// ----------------------------------------------------------------------------

int bitReverse(int i, int N)
{
    int j = i;
    int M = N;
    int Sum = 0;
    int W = 1;
    M = M / 2;
    while (M != 0)
    {
        j = (i & M) > M - 1;
        Sum += j * W;
        W *= 2;
        M = M / 2;
    }
    return Sum;
}

void computeWeight(int N, int k, float &Wr, float &Wi)
{
    Wr = cosl(2.0 * M_PI * k / float(N));
    Wi = sinl(2.0 * M_PI * k / float(N));
}

float *computeButterflyLookupTexture()
{
    float *data = new float[FFT_SIZE * PASSES * 4];

    for (int i = 0; i < PASSES; i++)
    {
        int nBlocks  = (int) powf(2.0, float(PASSES - 1 - i));
        int nHInputs = (int) powf(2.0, float(i));
        for (int j = 0; j < nBlocks; j++)
        {
            for (int k = 0; k < nHInputs; k++)
            {
                int i1, i2, j1, j2;
                if (i == 0)
                {
                    i1 = j * nHInputs * 2 + k;
                    i2 = j * nHInputs * 2 + nHInputs + k;
                    j1 = bitReverse(i1, FFT_SIZE);
                    j2 = bitReverse(i2, FFT_SIZE);
                }
                else
                {
                    i1 = j * nHInputs * 2 + k;
                    i2 = j * nHInputs * 2 + nHInputs + k;
                    j1 = i1;
                    j2 = i2;
                }

                float wr, wi;
                computeWeight(FFT_SIZE, k * nBlocks, wr, wi);

                int offset1 = 4 * (i1 + i * FFT_SIZE);
                data[offset1 + 0] = (j1 + 0.5) / FFT_SIZE;
                data[offset1 + 1] = (j2 + 0.5) / FFT_SIZE;
                data[offset1 + 2] = wr;
                data[offset1 + 3] = wi;

                int offset2 = 4 * (i2 + i * FFT_SIZE);
                data[offset2 + 0] = (j1 + 0.5) / FFT_SIZE;
                data[offset2 + 1] = (j2 + 0.5) / FFT_SIZE;
                data[offset2 + 2] = -wr;
                data[offset2 + 3] = -wi;
            }
        }
    }

    return data;
}

void simulateFFTWaves(float t)
{
    // init
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FFT0]);
    for (int i = 0; i < 8; ++i)
	{
		glFramebufferTextureLayerEXT(   GL_FRAMEBUFFER_EXT,
										GL_COLOR_ATTACHMENT0_EXT + i,
										gl::textures[gl::texture::FFT_PING],
										0,
										i);
	}
	GLenum drawBuffers[8] =
    {
        GL_COLOR_ATTACHMENT0_EXT,
        GL_COLOR_ATTACHMENT1_EXT,
        GL_COLOR_ATTACHMENT2_EXT,
        GL_COLOR_ATTACHMENT3_EXT,
        GL_COLOR_ATTACHMENT4_EXT,
        GL_COLOR_ATTACHMENT5_EXT,
        GL_COLOR_ATTACHMENT6_EXT,
        GL_COLOR_ATTACHMENT7_EXT
    };
    glDrawBuffers(choppy ? 8 : 3, drawBuffers);
    glViewport(0, 0, FFT_SIZE, FFT_SIZE);

    glUseProgram(gl::programs[gl::program::INIT]->program);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::INIT]->program, "FFT_SIZE"),FFT_SIZE);
    glUniform4f(glGetUniformLocation(gl::programs[gl::program::INIT]->program, "INVERSE_GRID_SIZES"),
                2.0 * M_PI * FFT_SIZE / GRID1_SIZE,
                2.0 * M_PI * FFT_SIZE / GRID2_SIZE,
                2.0 * M_PI * FFT_SIZE / GRID3_SIZE,
                2.0 * M_PI * FFT_SIZE / GRID4_SIZE);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::INIT]->program, "t"), t);
    drawQuad();

    // fft passes
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FFT1]);
//    glClearColor(1.0,0.0,0.0,0.0);
//    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(gl::programs[gl::program::FFTX]->program);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "nLayers"), choppy ? 8 : 3);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "sLayer"), 0);
    for (int i = 0; i < PASSES; ++i)
    {
        glUniform1f(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "pass"), float(i + 0.5) / PASSES);
        if (i%2 == 0)
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "imgSampler"), gl::texture::FFT_PING);
            glDrawBuffer(GL_COLOR_ATTACHMENT1_EXT);
        }
        else
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "imgSampler"), gl::texture::FFT_PONG);
            glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        }
        drawQuad();
    }


    glUseProgram(gl::programs[gl::program::FFTY]->program);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "nLayers"), choppy ? 8 : 3);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "sLayer"), 0);
    for (int i = PASSES; i < 2 * PASSES; ++i)
    {
        glUniform1f(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "pass"), float(i - PASSES + 0.5) / PASSES);
        if (i%2 == 0)
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "imgSampler"), gl::texture::FFT_PING);
            glDrawBuffer(GL_COLOR_ATTACHMENT1_EXT);
        }
        else
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "imgSampler"), gl::texture::FFT_PONG);
            glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        }
        drawQuad();
    }

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

//	fftPass = 1 - fftPass;
}

void simulateFFTWaves2(float t)
{
    // init
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FFT0]);
    for (int i = 0; i < 1; ++i)
	{
		glFramebufferTextureLayerEXT(   GL_FRAMEBUFFER_EXT,
										GL_COLOR_ATTACHMENT0_EXT + i,
										gl::textures[gl::texture::FFT_PING],
										0,
										8+i);
	}
	GLenum drawBuffers[2] =
    {
        GL_COLOR_ATTACHMENT0_EXT,
        GL_COLOR_ATTACHMENT1_EXT,
    };
    glDrawBuffers(2, drawBuffers);

    glViewport(0, 0, FFT_SIZE, FFT_SIZE);

    glUseProgram(gl::programs[gl::program::INIT2]->program);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::INIT2]->program, "FFT_SIZE"),FFT_SIZE);
    glUniform4f(glGetUniformLocation(gl::programs[gl::program::INIT2]->program, "INVERSE_GRID_SIZES"),
                2.0 * M_PI * FFT_SIZE / GRID1_SIZE,
                2.0 * M_PI * FFT_SIZE / GRID2_SIZE,
                2.0 * M_PI * FFT_SIZE / GRID3_SIZE,
                2.0 * M_PI * FFT_SIZE / GRID4_SIZE);
    glUniform1f(glGetUniformLocation(gl::programs[gl::program::INIT2]->program, "t"), t);
    drawQuad();

    // fft passes
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FFT1]);
//    glClearColor(0.0,0.0,0.0,0.0);
//    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(gl::programs[gl::program::FFTX]->program);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "sLayer"), 8);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "nLayers"), 1);
    for (int i = 0; i < PASSES; ++i)
    {
        glUniform1f(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "pass"), float(i + 0.5) / PASSES);
        if (i%2 == 0)
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "imgSampler"), gl::texture::FFT_PING);
            glDrawBuffer(GL_COLOR_ATTACHMENT1_EXT);
        }
        else
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTX]->program, "imgSampler"), gl::texture::FFT_PONG);
            glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        }
        drawQuad();
    }
    glUseProgram(gl::programs[gl::program::FFTY]->program);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "sLayer"), 8);
    glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "nLayers"), 1);
    for (int i = PASSES; i < 2 * PASSES; ++i)
    {
        glUniform1f(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "pass"), float(i - PASSES + 0.5) / PASSES);
        if (i%2 == 0)
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "imgSampler"), gl::texture::FFT_PING);
            glDrawBuffer(GL_COLOR_ATTACHMENT1_EXT);
        }
        else
        {
            glUniform1i(glGetUniformLocation(gl::programs[gl::program::FFTY]->program, "imgSampler"), gl::texture::FFT_PONG);
            glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        }
        drawQuad();
    }

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

	fftPass = 1 - fftPass;
}

void TW_CALL getFloat(void *value, void *clientData)
{
    *((float*) value) = *((float*) clientData);
}

void TW_CALL setFloat(const void *value, void *clientData)
{
    *((float*) clientData) = *((float*) value);
    generateWavesSpectrum();
}

void TW_CALL getInt(void *value, void *clientData)
{
    *((int*) value) = *((int*) clientData);
}

void TW_CALL setInt(const void *value, void *clientData)
{
    *((int*) clientData) = *((int*) value);
    generateWavesSpectrum();
}

void TW_CALL getBool2(void *value, void *clientData)
{
    *((bool*) value) = *((bool*) clientData);
}

void TW_CALL setBool2(const void *value, void *clientData)
{
    *((bool*) clientData) = *((bool*) value);
    generateWavesSpectrum();
}


// ----------------------------------------------------------------------------

// scale a perspective frustum uniformly by a factor of l
void scaleFrustum(float l, float fovy, float aspect, float zNear, float zFar, const mat4f& view, mat4f& scaledView, mat4f& scaledProj)
{
	// compute backward translation
	float back = l / sin(fovy*M_PI/360.0);
	// compute new projection attributes
	float near2 = zNear - l + back;
	float far2 	= zFar + l + back;

	scaledProj = mat4f::perspectiveProjection(fovy, aspect, near2, far2);
	mat4f translate = mat4f( 1.0f, 0.0f, 0.0f, 0.0f,
							 0.0f, 1.0f, 0.0f, 0.0f,
							 0.0f, 0.0f, 1.0f, -back,
							 0.0f, 0.0f, 0.0f, 1.0f );

	scaledView = translate * view; // row major transform

//	cout << "near/far : "<< near2 << " " << far2 << endl;
}

void save(int id)
{
    char buf[256];
    sprintf(buf, "data/scene%d.dat", id);
    std::ofstream out;
    out.open(buf);
    out << cloudLayer << std::endl;
    out << octaves << std::endl;
    out << lacunarity << std::endl;
    out << gain << std::endl;
    out << norm << std::endl;
    out << clamp1 << std::endl;
    out << clamp2 << std::endl;
    out << cloudColor[0] << std::endl;
    out << cloudColor[1] << std::endl;
    out << cloudColor[2] << std::endl;
    out << cloudColor[3] << std::endl;
    out << sunTheta << std::endl;
    out << sunPhi << std::endl;
    out << camera::z << std::endl;
    out << camera::theta << std::endl;
    out << camera::phi << std::endl;
    out << gridSize << std::endl;
    out << seaColor[0] << std::endl;
    out << seaColor[1] << std::endl;
    out << seaColor[2] << std::endl;
    out << seaColor[3] << std::endl;
    out << hdrExposure << std::endl;
    out << grid << std::endl;
    out << animate << std::endl;
    out << seaContrib << std::endl;
    out << sunContrib << std::endl;
    out << skyContrib << std::endl;
    out << foamContrib << std::endl;
    out << manualFilter << std::endl;
    out << choppy << std::endl;
    out << GRID1_SIZE << std::endl;
    out << GRID2_SIZE << std::endl;
    out << GRID3_SIZE << std::endl;
    out << GRID4_SIZE << std::endl;
    out << WIND << std::endl;
    out << OMEGA << std::endl;
    out << propagate << std::endl;
    out << A << std::endl;
    out << choppy_factor0 << std::endl;
    out << choppy_factor1 << std::endl;
    out << choppy_factor2 << std::endl;
    out << choppy_factor3 << std::endl;
    out << jacobian_scale << std::endl;
    out.close();
}

void load(int id)
{
    char buf[256];
    sprintf(buf, "data/scene%d.dat", id);
    std::ifstream in;
    in.open(buf);

    in >> cloudLayer;
    in >> octaves;
    in >> lacunarity;
    in >> gain;
    in >> norm;
    in >> clamp1;
    in >> clamp2;
    in >> cloudColor[0];
    in >> cloudColor[1];
    in >> cloudColor[2];
    in >> cloudColor[3];
    in >> sunTheta;
    in >> sunPhi;
    in >> camera::z;
    in >> camera::theta;
    in >> camera::phi;
    in >> gridSize;
    in >> seaColor[0];
    in >> seaColor[1];
    in >> seaColor[2];
    in >> seaColor[3];
    in >> hdrExposure;
    in >> grid;
    in >> animate;
    in >> seaContrib;
    in >> sunContrib;
    in >> skyContrib;
    in >> foamContrib;
    in >> manualFilter;
    in >> choppy;
    in >> GRID1_SIZE;
    in >> GRID2_SIZE;
    in >> GRID3_SIZE;
    in >> GRID4_SIZE;
    in >> WIND;
    in >> OMEGA;
    in >> propagate;
    in >> A;
    in >> choppy_factor0;
    in >> choppy_factor1;
    in >> choppy_factor2;
    in >> choppy_factor3;
    in >> jacobian_scale;
    in.close();
    generateMesh();
    generateWavesSpectrum();
    computeSlopeVarianceTex(NULL);
    loadPrograms(true);
    TwRefreshBar(tw::bar);
}

// get time in seconds
double time()
{
#ifdef _WIN32
    __int64 time;
    __int64 cpuFrequency;
    QueryPerformanceCounter((LARGE_INTEGER*) &time);
    QueryPerformanceFrequency((LARGE_INTEGER*) &cpuFrequency);
    return time / double(cpuFrequency);
#else
    static double t0 = 0;
    timeval tv;
    gettimeofday(&tv, NULL);
    if (!t0)
    {
        t0 = tv.tv_sec;
    }
    return double(tv.tv_sec-t0) + double(tv.tv_usec) / 1e6;
#endif
}


void drawTessCube(float tess)
{
    const float val = 1.0f/tess;
    glBegin(GL_QUADS);
    for(float j = -1.0f; j < 1.0f; j+=val)
        for(float i = -1.0f; i < 1.0f; i+=val)
        {
            glVertex3f(i, 1.0f, j+val);
            glVertex3f(i+val, 1.0f, j+val);
            glVertex3f(i+val, 1.0f, j);
            glVertex3f(i, 1.0f, j);
        }
    for(float j = -1.0f; j < 1.0f; j+=val)
        for(float i = -1.0f; i < 1.0f; i+=val)
        {
            glVertex3f(i, -1.0f, j);
            glVertex3f(i+val, -1.0f, j);
            glVertex3f(i+val, -1.0f, j+val);
            glVertex3f(i, -1.0f, j+val);
        }

    for(float j = -1.0f; j < 1.0f; j+=val)
        for(float i = -1.0f; i < 1.0f; i+=val)
        {
            glVertex3f(1.0f, i, j);
            glVertex3f(1.0f, i+val,  j);
            glVertex3f(1.0f, i+val, j+val);
            glVertex3f(1.0f, i, j+val);
        }
    for(float j = -1.0f; j < 1.0f; j+=val)
        for(float i = -1.0f; i < 1.0f; i+=val)
        {
            glVertex3f(-1.0f, i, j+val);
            glVertex3f(-1.0f, i+val, j+val);
            glVertex3f(-1.0f, i+val,  j);
            glVertex3f(-1.0f, i, j);
        }
    for(float j = -1.0f; j < 1.0f; j+=val)
        for(float i = -1.0f; i < 1.0f; i+=val)
        {
            glVertex3f(i, j+val, -1.0f);
            glVertex3f(i+val, j+val, -1.0f);
            glVertex3f(i+val, j, -1.0f);
            glVertex3f(i, j, -1.0f);
        }
    for(float j = -1.0f; j < 1.0f; j+=val)
        for(float i = -1.0f; i < 1.0f; i+=val)
        {
            glVertex3f(i, j, 1.0f);
            glVertex3f(i+val, j, 1.0f);
            glVertex3f(i+val, j+val, 1.0f);
            glVertex3f(i, j+val, 1.0f);
        }

    glEnd();
}

void redisplayFunc()
{
//cout << glu::errorString(glGetError()) << endl;

	static double t0 = 0.0;
	static double t1 = time();

	t0          = time();
	float delta = t0 - t1;

	bencher += delta;
	++frames;

	camera::x += camera::vel * delta * camera::velx * max(camera::z*0.5f,1.0f);
	camera::y += camera::vel * delta * camera::vely * max(camera::z*0.5f,1.0f);
	camera::z += camera::vel * delta * camera::velz * max(camera::z*0.5f,1.0f);
	if(camera::z  < 1.0)
		camera::z = 1.0;

	if (vboParams.x != window::width || vboParams.y != window::height || vboParams.z != gridSize || vboParams.w != camera::theta)
	{
		generateMesh();
		vboParams.x = window::width;
		vboParams.y = window::height;
		vboParams.z = gridSize;
		vboParams.w = camera::theta;
	}

	glClearColor(1.0,1.0,1.0,1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	vec4f sun = vec4f(sin(sunTheta) * cos(sunPhi), sin(sunTheta) * sin(sunPhi), cos(sunTheta), 0.0);

	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glDisable(GL_DEPTH_TEST);

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::SKY]);
	glViewport(0, 0, skyTexSize, skyTexSize);
	glUseProgram(gl::programs[gl::program::SKYMAP]->program);
	glUniform3f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "sunDir"), sun.x, sun.y, sun.z);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "octaves"), octaves);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "lacunarity"), lacunarity);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "gain"), gain);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "norm"), norm);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "clamp1"), clamp1);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "clamp2"), clamp2);
	glUniform4f(glGetUniformLocation(gl::programs[gl::program::SKYMAP]->program, "cloudsColor"), cloudColor[0], cloudColor[1], cloudColor[2], cloudColor[3]);
	glBegin(GL_TRIANGLE_STRIP);
	glVertex2f(-1, -1);
	glVertex2f(1, -1);
	glVertex2f(-1, 1);
	glVertex2f(1, 1);
	glEnd();
	glActiveTexture(GL_TEXTURE0 + gl::texture::SKY);
	glGenerateMipmapEXT(GL_TEXTURE_2D);

	// update wave heights
	static double t = 0.0;
	if(animate)
		t += delta*speed;

	// solve fft
    simulateFFTWaves(t);
//    simulateFFTWaves2(t);
    glActiveTexture(GL_TEXTURE0 + gl::texture::FFT_PING);
		glGenerateMipmapEXT(GL_TEXTURE_2D_ARRAY_EXT);

	/// filtering
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::GAUSS]);
	glViewport(0, 0, FFT_SIZE, FFT_SIZE);
	glUseProgram(gl::programs[gl::program::GAUSS]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::GAUSS]->program, "fftWavesSampler"), gl::texture::FFT_PING);
	glUniform4f(glGetUniformLocation(gl::programs[gl::program::GAUSS]->program, "choppy"), choppy_factor0, choppy_factor1, choppy_factor2, choppy_factor3);

	drawQuad();
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

	glActiveTexture(GL_TEXTURE0 + gl::texture::GAUSSZ);
	glGenerateMipmapEXT(GL_TEXTURE_2D_ARRAY_EXT);

	float ch = camera::z;

	mat4f proj = mat4f::perspectiveProjection(camera::fovy,
	                                          float(window::width)/float(window::height),
	                                          0.1 * ch,
	                                          300000.0 * ch);

    mat4f view = mat4f(
                     0.0, -1.0, 0.0, -camera::x,
                     0.0, 0.0, 1.0, -camera::z,
                     -1.0, 0.0, 0.0, -camera::y,
                     0.0, 0.0, 0.0, 1.0
                 );


	view = mat4f::rotatey(camera::phi) * view;
	view = mat4f::rotatex(camera::theta) * view;


	// compute foam
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FOAM]);
	glViewport(0, 0, window::width, window::height);

	float clear[] = {0.0,0.0,0.0,0.0};
	glClearBufferfv(GL_COLOR,0,clear);
	glClear(GL_DEPTH_BUFFER_BIT);

	glUseProgram(gl::programs[gl::program::FOAM]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "fftWavesSampler"), gl::texture::FFT_PING);
//	glUniform1i(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "prevfftWavesSampler"), gl::texture::FFT_PING_0 + fftPass);
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "invProjection"), 1, true, proj.inverse().coefficients());
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "invView"), 1, true, view.inverse().coefficients());
//	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "view"), 1, true, cam2d.coefficients());
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "mvp"), 1, true, (proj * view).coefficients());
	glUniform3f(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "camWorldPos"), view.inverse()[0][3], view.inverse()[1][3], view.inverse()[2][3]);
	glUniform4f(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "GRID_SIZES"), GRID1_SIZE, GRID2_SIZE, GRID3_SIZE, GRID4_SIZE);
	glUniform2f(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "gridSize"), gridSize / float(window::width), gridSize / float(window::height));
	glUniform4f(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "choppy_factor"), choppy_factor0,choppy_factor1,choppy_factor2,choppy_factor3);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::FOAM]->program, "jacobian_scale"), jacobian_scale);


	glBindBuffer(GL_ARRAY_BUFFER, gl::buffers[gl::buffer::VERTEX_GRID]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl::buffers[gl::buffer::INDEX_GRID]);
	glVertexPointer(4, GL_FLOAT, 16, 0);
	glEnableClientState(GL_VERTEX_ARRAY);
	glDrawElements(GL_TRIANGLES, vboSize, GL_UNSIGNED_INT, 0);


	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
	glDrawBuffer(GL_BACK);
	glViewport(0, 0, window::width, window::height);
	glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

	if (show_spectrum)
	{
		glUseProgram(gl::programs[gl::program::SHOW_SPECTRUM]->program);
		glUniform4f(glGetUniformLocation(gl::programs[gl::program::SHOW_SPECTRUM]->program, "INVERSE_GRID_SIZES"),
		            M_PI * FFT_SIZE / GRID1_SIZE,
		            M_PI * FFT_SIZE / GRID2_SIZE,
		            M_PI * FFT_SIZE / GRID3_SIZE,
		            M_PI * FFT_SIZE / GRID4_SIZE);
		glUniform1f(glGetUniformLocation(gl::programs[gl::program::SHOW_SPECTRUM]->program, "FFT_SIZE"), FFT_SIZE);
		glUniform1f(glGetUniformLocation(gl::programs[gl::program::SHOW_SPECTRUM]->program, "zoom"), show_spectrum_zoom);
		glUniform1f(glGetUniformLocation(gl::programs[gl::program::SHOW_SPECTRUM]->program, "linear"), show_spectrum_linear);
		drawQuad();
		TwDraw();
		glutSwapBuffers();
		t1 = t0;
		return;
	}

/// Final Rendering
	glEnable(GL_DEPTH_TEST);
	glUseProgram(gl::programs[gl::program::RENDER]->program);
	glUniform1i(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "fftWavesSampler"), gl::texture::FFT_PING);
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "screenToCamera"), 1, true, proj.inverse().coefficients());
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "cameraToWorld"), 1, true, view.inverse().coefficients());
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "worldToScreen"), 1, true, (proj * view).coefficients());
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "worldDirToScreen"), 1, true, (proj * view).coefficients());
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "modelView"), 1, true, view.coefficients());
	glUniform3f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "worldCamera"),  view.inverse()[0][3], view.inverse()[1][3], view.inverse()[2][3]);
	glUniform3f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "worldSunDir"), sun.x, sun.y, sun.z);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "hdrExposure"), hdrExposure);

	glUniform3f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "seaColor"), seaColor[0] * seaColor[3], seaColor[1] * seaColor[3], seaColor[2] * seaColor[3]);

	glUniform4f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "GRID_SIZES"), GRID1_SIZE, GRID2_SIZE, GRID3_SIZE, GRID4_SIZE);

	glUniform2f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "gridSize"), gridSize/float(window::width), gridSize/float(window::height));
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "spectrum"), show_spectrum);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "normals"), normals);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "choppy"), choppy);
	glUniform4f(glGetUniformLocation(gl::programs[gl::program::RENDER]->program, "choppy_factor"),choppy_factor0,choppy_factor1,choppy_factor2,choppy_factor3);

	if (grid)
	{
		glPolygonMode(GL_FRONT, GL_LINE);
		glPolygonMode(GL_BACK, GL_LINE);
	}
	else
	{
		glPolygonMode(GL_FRONT, GL_FILL);
		glPolygonMode(GL_BACK, GL_FILL);
	}

	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);

	glBindBuffer(GL_ARRAY_BUFFER, gl::buffers[gl::buffer::VERTEX_GRID]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl::buffers[gl::buffer::INDEX_GRID]);
	glVertexPointer(4, GL_FLOAT, 16, 0);
	glEnableClientState(GL_VERTEX_ARRAY);
	//    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	glDrawElements(GL_TRIANGLES, vboSize, GL_UNSIGNED_INT, 0);
	//    glDisable(GL_DEPTH_TEST);
	//    glPointSize(3.0f);
	//    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
	//    glDrawElements(GL_TRIANGLES, vboSize, GL_UNSIGNED_INT, 0);

	glPolygonMode(GL_FRONT, GL_FILL);
	glPolygonMode(GL_BACK, GL_FILL);
	glDisable(GL_CULL_FACE);

	glDisableClientState(GL_VERTEX_ARRAY);

	if (cloudLayer && ch > 3000.0)
	{
		drawClouds(sun, proj * view);
	}


	/// render atmosphere (after scene -> use early Z !!)
	glUseProgram(gl::programs[gl::program::SKY]->program);
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "screenToCamera"), 1, true, proj.inverse().coefficients());
	glUniformMatrix4fv(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "cameraToWorld"), 1, true, view.inverse().coefficients());
	glUniform3f(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "worldCamera"), 0.0f, 0.0f, ch);
	glUniform3f(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "worldSunDir"), sun.x, sun.y, sun.z);
	glUniform1f(glGetUniformLocation(gl::programs[gl::program::SKY]->program, "hdrExposure"), hdrExposure);
	glBegin(GL_TRIANGLE_STRIP);
	glVertex2f(-1, -1);
	glVertex2f(1, -1);
	glVertex2f(-1, 1);
	glVertex2f(1, 1);
	glEnd();

	if (cloudLayer && camera::z < 3000.0)
		drawClouds(sun, proj * view);


	glUseProgram(0);

	TwDraw();
	glutSwapBuffers();

	t1 = t0;
}

void perf() {
	cout << "avg time : " << bencher*1000.0 / frames << " ms" << endl;
}

void reshapeFunc(int x, int y) {
	window::width = x;
	window::height = y;
	TwWindowSize(x, y);
	glutPostRedisplay();
}

void keyboardFunc(unsigned char c, int x, int y) {
	if (TwEventKeyboardGLUT(c, x, y))
	{
		return;
	}

	if(c == 27)
	{
		::exit(0);
	}
	if (c == '+')
	{
		camera::theta = min(camera::theta + 5.0f, 90.0f - 0.001f);
	}
	if (c == '-')
	{
		camera::theta = camera::theta - 5.0;
		if(camera::theta < -45.0)
			camera::theta = -45.0;
	}
	if (c >= '1' && c <= '9')
	{
		save(c - '0');
	}
	if (c == 'z')
	{
		camera::vely = max(-1.0f, camera::vely - 1.0f);
	}
	if (c == 's')
	{
		camera::vely = min(1.0f, camera::vely + 1.0f);
	}
	if (c == 'q')
	{
		camera::velx = max(-1.0f, camera::velx - 1.0f);
	}
	if (c == 'd')
	{
		camera::velx = min(1.0f, camera::velx + 1.0f);
	}
	if (c == 'e')
	{
		camera::velz = max(-1.0f, camera::velz - 1.0f);
	}
	if (c == 'a')
	{
		camera::velz = min(1.0f, camera::velz + 1.0f);
	}
}


void keyboardUpFunc(unsigned char c, int x, int y) {
	if (c == 'z')
	{
		camera::vely = min(1.0f, camera::vely + 1.0f);
	}
	if (c == 's')
	{
		camera::vely = max(-1.0f, camera::vely - 1.0f);
	}
	if (c == 'q')
	{
		camera::velx = min(1.0f, camera::velx + 1.0f);
	}
	if (c == 'd')
	{
		camera::velx = max(-1.0f, camera::velx - 1.0f);
	}
	if (c == 'e')
	{
		camera::velz = min(1.0f, camera::velz + 1.0f);
	}
	if (c == 'a')
	{
		camera::velz = max(-1.0f, camera::velz - 1.0f);
	}
}

void specialKeyFunc(int c, int x, int y) {
	if (c >= GLUT_KEY_F1 && c <= GLUT_KEY_F9)
	{
		load(c - GLUT_KEY_F1 + 1);
	}
	switch (c)
	{
	case GLUT_KEY_LEFT:
		camera::phi -= 5.0;
		break;
	case GLUT_KEY_RIGHT:
		camera::phi += 5.0;
		break;
	}
}

int oldx;
int oldy;
bool drag;

void mouseClickFunc(int b, int s, int x, int y) {
	drag = false;
	if (!TwEventMouseButtonGLUT(b, s, x, y) && b == 0)
	{
		oldx = x;
		oldy = y;
		drag = true;
	}
}

void mouseMotionFunc(int x, int y) {
	if (drag)
	{
		sunPhi += (oldx - x) / 400.0;
		sunTheta += (y - oldy) / 400.0;
		oldx = x;
		oldy = y;
	}
	else
	{
		TwMouseMotion(x, y);
	}
}

void mousePassiveMotionFunc(int x, int y) {
	TwMouseMotion(x, y);
}

void idleFunc() {
	glutPostRedisplay();
}

void onClean() {
	GLenum error = glGetError();
	if(error)
		std::cout << "GL_ERROR : "
		          << glu::errorString(error)
		          << " during runtime\n";

	for(int i = 0; i < gl::program::MAX; ++i)
		delete gl::programs[i];

	glDeleteBuffers(gl::buffer::MAX, gl::buffers);
	glDeleteTextures(gl::texture::MAX, gl::textures);
	glDeleteFramebuffersEXT(gl::fbuffer::MAX, gl::fbuffers);
	glDeleteRenderbuffersEXT(gl::rbuffer::MAX, gl::rbuffers);

	glFinish();
	error = glGetError();
	if(error)
		std::cout << "GL_ERROR : "
		          << error
		          << ": some objects might not have been cleaned...\n";
	else
		std::cout << "Exited Cleanly\n";
}

int main(int argc, char* argv[]) {
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
	glutInitWindowSize(window::width, window::height);
	glutCreateWindow("Ocean");
	glutCreateMenu(NULL);

//	glutSetKeyRepeat(GLUT_KEY_REPEAT_OFF);
//	glutIgnoreKeyRepeat(1);

	glutDisplayFunc(redisplayFunc);
	glutReshapeFunc(reshapeFunc);
	glutKeyboardFunc(keyboardFunc);
	glutKeyboardUpFunc(keyboardUpFunc);
	glutSpecialFunc(specialKeyFunc);
	glutMouseFunc(mouseClickFunc);
	glutMotionFunc(mouseMotionFunc);
	glutPassiveMotionFunc(mousePassiveMotionFunc);
	glutIdleFunc(idleFunc);
	glewInit();

	TwInit(TW_OPENGL, NULL);
	TwGLUTModifiersFunc(glutGetModifiers);

	tw::bar = TwNewBar("Parameters");
	TwDefine(" Parameters size='220 600' ");

	TwAddVarCB(tw::bar, "L1", TW_TYPE_FLOAT, setFloat, getFloat, &GRID1_SIZE, "min=1.0 max=50000.0 step=1.0 group=Spectrum");
	TwAddVarCB(tw::bar, "L2", TW_TYPE_FLOAT, setFloat, getFloat, &GRID2_SIZE, "min=1.0 max=50000.0 step=1.0 group=Spectrum");
	TwAddVarCB(tw::bar, "L3", TW_TYPE_FLOAT, setFloat, getFloat, &GRID3_SIZE, "min=1.0 max=50000.0 step=1.0 group=Spectrum");
	TwAddVarCB(tw::bar, "L4", TW_TYPE_FLOAT, setFloat, getFloat, &GRID4_SIZE, "min=1.0 max=50000.0 step=1.0 group=Spectrum");
	TwAddVarCB(tw::bar, "Wind speed", TW_TYPE_FLOAT, setFloat, getFloat, &WIND, "min=1.0 max=30.0 step=1.0 group=Spectrum");
	TwAddVarCB(tw::bar, "Inv. wave age", TW_TYPE_FLOAT, setFloat, getFloat, &OMEGA, "min=0.84 max=4.99 step=0.01 group=Spectrum");
	TwAddVarCB(tw::bar, "Propagate", TW_TYPE_BOOL8, setBool2, getBool2, &propagate, "group=Spectrum");
	TwAddVarCB(tw::bar, "Amplitude", TW_TYPE_FLOAT, setFloat, getFloat, &A, "min=0.01 max=1000.0 step=0.01 group=Spectrum");
	TwAddButton(tw::bar, "Generate", computeSlopeVarianceTex, NULL, "group=Spectrum");

	TwAddVarRW(tw::bar, "Altitude", TW_TYPE_FLOAT, &camera::z, "min=-10.0 max=8000 group=Rendering");
	TwAddVarRO(tw::bar, "Theta", TW_TYPE_FLOAT, &camera::theta, "group=Rendering");
	TwAddVarRO(tw::bar, "Phi", TW_TYPE_FLOAT, &camera::phi, "group=Rendering");
	TwAddVarRW(tw::bar, "Grid size", TW_TYPE_FLOAT, &gridSize, "min=1.0 max=16.0 step=1.0 group=Rendering");
	TwAddVarRW(tw::bar, "Sea color", TW_TYPE_COLOR4F, &seaColor, "group=Rendering");
	TwAddVarRW(tw::bar, "Exposure", TW_TYPE_FLOAT, &hdrExposure, "min=0.01 max=4.0 step=0.01 group=Rendering");
	TwAddVarRW(tw::bar, "Animation", TW_TYPE_BOOLCPP, &animate, "group=Rendering");
	TwAddVarRW(tw::bar, "Speed", TW_TYPE_FLOAT, &speed, "group=Rendering min=-2.000 max=2.000 step=0.025");
	TwAddVarRW(tw::bar, "Grid", TW_TYPE_BOOL8, &grid, "group=Rendering");
	TwAddVarRW(tw::bar, "Spectrum_", TW_TYPE_BOOL8, &show_spectrum, "label=Spectrum group=Rendering");
	TwAddVarRW(tw::bar, "Spectrum Zoom", TW_TYPE_FLOAT, &show_spectrum_zoom, "min=0.0 max=1.0 step=0.01 group=Rendering");
	TwAddVarRW(tw::bar, "Spectrum Linear", TW_TYPE_BOOL8, &show_spectrum_linear, "group=Rendering");
	TwAddVarRW(tw::bar, "Normals", TW_TYPE_BOOLCPP, &normals, "group=Rendering");
	TwAddVarRW(tw::bar, "Choppy", TW_TYPE_BOOLCPP, &choppy, "group=Rendering");
	TwAddVarRW(tw::bar, "ChoppyFactor0", TW_TYPE_FLOAT, &choppy_factor0, "min=0.0 max=100.0 step=0.1 group=Rendering");
	TwAddVarRW(tw::bar, "ChoppyFactor1", TW_TYPE_FLOAT, &choppy_factor1, "min=0.0 max=100.0 step=0.1 group=Rendering");
	TwAddVarRW(tw::bar, "ChoppyFactor3", TW_TYPE_FLOAT, &choppy_factor2, "min=0.0 max=100.0 step=0.1 group=Rendering");
	TwAddVarRW(tw::bar, "ChoppyFactor4", TW_TYPE_FLOAT, &choppy_factor3, "min=0.0 max=100.0 step=0.1 group=Rendering");
	TwAddVarCB(tw::bar, "Sea", TW_TYPE_BOOLCPP, setBool, getBool, &seaContrib, "group=Rendering");
	TwAddVarCB(tw::bar, "Sun", TW_TYPE_BOOLCPP, setBool, getBool, &sunContrib, "group=Rendering");
	TwAddVarCB(tw::bar, "Sky", TW_TYPE_BOOLCPP, setBool, getBool, &skyContrib, "group=Rendering");
	TwAddVarCB(tw::bar, "Whitecaps", TW_TYPE_BOOLCPP, setBool, getBool, &foamContrib, "group=Rendering");
	TwAddVarCB(tw::bar, "Manual filter", TW_TYPE_BOOLCPP, setBool, getBool, &manualFilter, "group=Rendering");

	TwAddVarRW(tw::bar, "JScale", TW_TYPE_FLOAT, &jacobian_scale, "min=-50.0 max=50.0 step=0.001 group=Whitecap");

	TwAddVarRW(tw::bar, "Octaves", TW_TYPE_FLOAT, &octaves, "min=1.0 max=16.0 step=1.0 group=Clouds");
	TwAddVarRW(tw::bar, "Lacunarity", TW_TYPE_FLOAT, &lacunarity, "min=0.1 max=3.0 step=0.1 group=Clouds");
	TwAddVarRW(tw::bar, "Gain", TW_TYPE_FLOAT, &gain, "min=0.01 max=2.0 step=0.01 group=Clouds");
	TwAddVarRW(tw::bar, "Norm", TW_TYPE_FLOAT, &norm, "min=0.01 max=1.0 step=0.01 group=Clouds");
	TwAddVarRW(tw::bar, "Clamp1", TW_TYPE_FLOAT, &clamp1, "min=-1.0 max=1.0 step=0.01 group=Clouds");
	TwAddVarRW(tw::bar, "Clamp2", TW_TYPE_FLOAT, &clamp2, "min=-1.0 max=1.0 step=0.01 group=Clouds");
	TwAddVarCB(tw::bar, "Enable1", TW_TYPE_BOOL8, setBool, getBool, &cloudLayer, "label=Enable group=Clouds");
	TwAddVarRW(tw::bar, "Color", TW_TYPE_COLOR4F, &cloudColor, "group=Clouds");

	for(GLuint i = 0; i < gl::program::MAX; ++i)
		gl::programs[i] = NULL;


    // Gen GL Objects
	glGenFramebuffersEXT(gl::fbuffer::MAX, gl::fbuffers);
	glGenTextures(gl::texture::MAX, gl::textures);
	glGenRenderbuffersEXT(gl::rbuffer::MAX, gl::rbuffers);
	glGenBuffers(gl::buffer::MAX, gl::buffers);

	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, gl::rbuffers[gl::rbuffer::DEPTH_FBO_JACOBIANS]);
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, window::width, window::height);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);

// Textures
	float *data = new float[16*64*3];
	FILE *f = fopen("data/irradiance.raw", "rb");
	fread(data, 1, 16*64*3*sizeof(float), f);
	fclose(f);
	glActiveTexture(GL_TEXTURE0 + gl::texture::IRRADIANCE);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::IRRADIANCE]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F_ARB, 64, 16, 0, GL_RGB, GL_FLOAT, data);
	delete[] data;

	int res = 64;
	int nr = res / 2;
	int nv = res * 2;
	int nb = res / 2;
	int na = 8;
	f = fopen("data/inscatter.raw", "rb");
	data = new float[nr*nv*nb*na*4];
	fread(data, 1, nr*nv*nb*na*4*sizeof(float), f);
	fclose(f);
	glActiveTexture(GL_TEXTURE0 + gl::texture::INSCATTER);
	glBindTexture(GL_TEXTURE_3D, gl::textures[gl::texture::INSCATTER]);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F_ARB, na*nb, nv, nr, 0, GL_RGBA, GL_FLOAT, data);
	delete[] data;

	data = new float[256*64*3];
	f = fopen("data/transmittance.raw", "rb");
	fread(data, 1, 256*64*3*sizeof(float), f);
	fclose(f);
	glActiveTexture(GL_TEXTURE0 + gl::texture::TRANSMITTANCE);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::TRANSMITTANCE]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F_ARB, 256, 64, 0, GL_RGB, GL_FLOAT, data);
	delete[] data;

	float maxAnisotropy = 1.0f;
	glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);

	glActiveTexture(GL_TEXTURE0 + gl::texture::SKY);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::SKY]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
//		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, skyTexSize, skyTexSize, 0, GL_RGBA, GL_FLOAT, NULL);
		glGenerateMipmapEXT(GL_TEXTURE_2D);

	unsigned char* img = new unsigned char[512 * 512 + 38];
	f = fopen("data/noise.pgm", "rb");
	fread(img, 1, 512 * 512 + 38, f);
	fclose(f);
	glActiveTexture(GL_TEXTURE0 + gl::texture::NOISE);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::NOISE]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
//		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 512, 512, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, img + 38);
		glGenerateMipmapEXT(GL_TEXTURE_2D);
    delete[] img;

	img = new unsigned char[256 * 256 + 38];
//    f = fopen("data/noise/noise_simple_C256.pgm", "rb");
//    f = fopen("data/noise/noise_marble_C256.pgm", "rb");
    f = fopen("data/noise/noise_iA_C256.pgm", "rb");
//    f = fopen("data/noise/noise_iA_C256_2.pgm", "rb");
//    f = fopen("data/noise/noise_A_C256.pgm", "rb");
	fread(img, 1, 256 * 256 + 38, f);
	fclose(f);
	glActiveTexture(GL_TEXTURE0 + gl::texture::FOAM_SURFACE);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::FOAM_SURFACE]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
//		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 256, 256, 0, GL_RED, GL_UNSIGNED_BYTE, img + 38);
		glGenerateMipmapEXT(GL_TEXTURE_2D);

	delete[] img;


	glActiveTexture(GL_TEXTURE0 + gl::texture::SPECTRUM12);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::SPECTRUM12]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, FFT_SIZE, FFT_SIZE, 0, GL_RGB, GL_FLOAT, NULL);

	glActiveTexture(GL_TEXTURE0 + gl::texture::SPECTRUM34);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::SPECTRUM34]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, FFT_SIZE, FFT_SIZE, 0, GL_RGB, GL_FLOAT, NULL);

	glActiveTexture(GL_TEXTURE0 + gl::texture::SLOPE_VARIANCE);
	glBindTexture(GL_TEXTURE_3D, gl::textures[gl::texture::SLOPE_VARIANCE]);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glTexImage3D(GL_TEXTURE_3D, 0, GL_LUMINANCE_ALPHA16F_ARB, N_SLOPE_VARIANCE, N_SLOPE_VARIANCE, N_SLOPE_VARIANCE, 0, GL_LUMINANCE_ALPHA, GL_FLOAT, NULL);

	glActiveTexture(GL_TEXTURE0 + gl::texture::FFT_PING);
	glBindTexture(GL_TEXTURE_2D_ARRAY_EXT, gl::textures[gl::texture::FFT_PING]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_WRAP_T, GL_REPEAT);
//		glTexParameterf(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
		glTexImage3D(GL_TEXTURE_2D_ARRAY_EXT, 0, GL_RGBA32F_ARB, FFT_SIZE, FFT_SIZE, 10, 0, GL_RGBA, GL_FLOAT, NULL); // 8 = 1 for y + 2 for slope + 2 for D + 3 for Jacobians (Jxx, Jyy, Jxy)
		glGenerateMipmapEXT(GL_TEXTURE_2D_ARRAY_EXT);

	glActiveTexture(GL_TEXTURE0 + gl::texture::FFT_PONG);
	glBindTexture(GL_TEXTURE_2D_ARRAY_EXT, gl::textures[gl::texture::FFT_PONG]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_WRAP_T, GL_REPEAT);
//		glTexParameterf(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
		glTexImage3D(GL_TEXTURE_2D_ARRAY_EXT, 0, GL_RGBA32F_ARB, FFT_SIZE, FFT_SIZE, 10, 0, GL_RGBA, GL_FLOAT, NULL);
		glGenerateMipmapEXT(GL_TEXTURE_2D_ARRAY_EXT);

	glActiveTexture(GL_TEXTURE0 + gl::texture::BUTTERFLY);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::BUTTERFLY]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		data = computeButterflyLookupTexture();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, FFT_SIZE, PASSES, 0, GL_RGBA, GL_FLOAT, data);
	delete[] data;

	glActiveTexture(GL_TEXTURE0 + gl::texture::FOAM);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::FOAM]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
//		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, window::width, window::height, 0, GL_RED, GL_FLOAT, NULL);
		glGenerateMipmapEXT(GL_TEXTURE_2D);

	glActiveTexture(GL_TEXTURE0 + gl::texture::GAUSSZ);
	glBindTexture(GL_TEXTURE_2D_ARRAY_EXT, gl::textures[gl::texture::GAUSSZ]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_WRAP_T, GL_REPEAT);
//		glTexParameterf(GL_TEXTURE_2D_ARRAY_EXT, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
		glTexImage3D(GL_TEXTURE_2D_ARRAY_EXT, 0, GL_RGBA16F_ARB, FFT_SIZE, FFT_SIZE, 8, 0, GL_RGBA, GL_FLOAT, NULL);
		glGenerateMipmapEXT(GL_TEXTURE_2D_ARRAY_EXT);

	BMPClass bmp;
	if(BMPLoad("data/noise/noise_iA_C256_2normals.bmp", bmp)!=BMPNOERROR)
		std::cout << "normal map loading failed\n";

	glActiveTexture(GL_TEXTURE0 + gl::texture::FOAM_NORMALMAP);
	glBindTexture(GL_TEXTURE_2D, gl::textures[gl::texture::FOAM_NORMALMAP]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, bmp.width, bmp.height, 0, GL_RGB, GL_UNSIGNED_BYTE, bmp.bytes);
		glGenerateMipmapEXT(GL_TEXTURE_2D);

	generateWavesSpectrum();

// FrameBuffers
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::VARIANCES]);
		glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FFT0]);
		GLenum drawBuffers[8] =
		{
			GL_COLOR_ATTACHMENT0_EXT,
			GL_COLOR_ATTACHMENT1_EXT,
			GL_COLOR_ATTACHMENT2_EXT,
			GL_COLOR_ATTACHMENT3_EXT,
			GL_COLOR_ATTACHMENT4_EXT,
			GL_COLOR_ATTACHMENT5_EXT,
			GL_COLOR_ATTACHMENT6_EXT,
			GL_COLOR_ATTACHMENT7_EXT
		};
		glDrawBuffers(8, drawBuffers);


	// Layered FBO (using geometry shader, set layers to render to)
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FFT1]);
		glFramebufferTextureEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, gl::textures[gl::texture::FFT_PING], 0);
		glFramebufferTextureEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT1_EXT, gl::textures[gl::texture::FFT_PONG], 0);

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::FOAM]);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, gl::rbuffers[gl::rbuffer::DEPTH_FBO_JACOBIANS]);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, gl::textures[gl::texture::FOAM], 0);
		GLenum drawBuffers_foam[1] = {
			GL_COLOR_ATTACHMENT0_EXT
		};

		glDrawBuffers(1, drawBuffers_foam);

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::GAUSS]);
		glDrawBuffers(8, drawBuffers);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, gl::textures[gl::texture::GAUSSZ], 0, 0);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT1_EXT, gl::textures[gl::texture::GAUSSZ], 0, 1);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT2_EXT, gl::textures[gl::texture::GAUSSZ], 0, 2);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT3_EXT, gl::textures[gl::texture::GAUSSZ], 0, 3);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT4_EXT, gl::textures[gl::texture::GAUSSZ], 0, 4);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT5_EXT, gl::textures[gl::texture::GAUSSZ], 0, 5);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT6_EXT, gl::textures[gl::texture::GAUSSZ], 0, 6);
		glFramebufferTextureLayerEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT7_EXT, gl::textures[gl::texture::GAUSSZ], 0, 7);

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::SKY]);
		glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, gl::textures[gl::texture::SKY], 0);

//	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl::fbuffers[gl::fbuffer::TILE]);
//		glDrawBuffers(1, drawBuffers);
//		glFramebufferTexture(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, gl::textures[gl::texture::TILE_FOAM], 0);

	// back to default framebuffer
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
		glDrawBuffer(GL_BACK);

	// Grid
	generateMesh();

	// Programs
	loadPrograms(true);

	// Slope
	computeSlopeVarianceTex(NULL);

//	GLint maxColorAttach = 0;
//	glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS_EXT, &maxColorAttach);
//	std::cout << "GL_MAX_COLOR_ATTACHMENTS_EXT	: " << maxColorAttach << "\n";
//
//	GLint maxDrawBuffers = 0;	// Should be the same as max_color_attach
//	glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
//	std::cout << "GL_MAX_DRAW_BUFFERS		: " << maxDrawBuffers << "\n";
//
//	GLint max3DTexSize = 0;
//	glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max3DTexSize);
//	std::cout << "GL_MAX_3D_TEXTURE_SIZE 		: " << max3DTexSize << "\n";
//
//	GLint64 maxVertices = 0;
//	glGetInteger64v(GL_MAX_ELEMENTS_INDICES, &maxVertices);
//	std::cout <<"GL_MAX_ELEMENTS_INDICES : " << maxVertices << std::endl;

//	GLint maxTexUnits = 0;
//	glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxTexUnits);
//	std::cout << "GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS : " << maxTexUnits << "\n";
//
//	GLint maxArrayTexLayers = 0;
//	glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayTexLayers);
//	std::cout << "GL_MAX_ARRAY_TEXTURE_LAYERS : " << maxArrayTexLayers << "\n";

//	GLint maxTextureUnits = 0;
//	glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
//	std::cout << "GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS : " << maxTextureUnits << "\n";

	// Check Init Errors
	glFinish();
	GLenum error = glGetError();
	if(error)
	{
		std::cout << "Init gave GL_ERROR : " << error << "\n";
		onClean();
		return -1;
	}

	atexit(onClean);
	atexit(perf);

	glutMainLoop();



	return 0;
}
