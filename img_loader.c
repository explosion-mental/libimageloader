#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "img_loader_private.h"

#ifndef NO_DIR_LOADER
#include "dir_loader.h"
#endif

#ifndef NO_STB_IMAGE_LOADER
#include "stb_image_loader.h"
#endif

#ifndef NO_SPNG_LOADER
#include "spng_loader.h"
#endif

#ifndef NO_IMLIB2_LOADER
#include "imlib2_loader.h"
#endif

#ifndef NO_ZIP_LOADER
#include "zip_loader.h"
#endif

#ifndef NO_CURL_LOADER
#include "curl_loader.h"
#endif

#ifndef NO_PIPE_LOADER
#include "pipe_loader.h"
#endif

#define RUN_FUNC(DATA, LOADER, FUNC) do { \
    if(getLoaderEnabled(DATA->LOADER)->FUNC)getLoaderEnabled(DATA->LOADER)->FUNC(data); \
    } while(0)

#define MULTI_LOADER   (1 << 0)
#define NO_SEEK        (1 << 1)
#define NO_FD          (1 << 2)

typedef struct ImageLoader {
    const char* name;
    int (*img_open)(ImageContext*, int fd, ImageData*);
    void (*img_close)(ImageData*);
    char flags;
} ImageLoader;
#define CREATE_LOADER(NAME) { # NAME, NAME ## _load, .img_close= NAME ## _close}
#define CREATE_PARENT_LOADER(NAME, FLAGS){  # NAME , NAME ## _load, .flags = FLAGS}

static const ImageLoader img_loaders[] = {
#ifndef NO_DIR_LOADER
    CREATE_PARENT_LOADER(dir, MULTI_LOADER | NO_SEEK),
#endif
#ifndef NO_SPNG_LOADER
    CREATE_LOADER(spng),
#endif
#ifndef NO_STB_IMAGE_LOADER
    CREATE_LOADER(stb_image),
#endif
#ifndef NO_ZIP_LOADER
    CREATE_PARENT_LOADER(zip, MULTI_LOADER),
#endif
#ifndef NO_IMLIB2_LOADER
    CREATE_LOADER(imlib2),
#endif
#ifndef NO_CURL_LOADER
    CREATE_PARENT_LOADER(curl, MULTI_LOADER | NO_FD | NO_SEEK),
#endif
};

static ImageLoader pipe_loader = CREATE_PARENT_LOADER(pipe, MULTI_LOADER | NO_SEEK);

int getFD(ImageData* data) {
    int fd = data->fd;
    if(fd == -1)
        fd = open(data->name, O_RDONLY | O_CLOEXEC);
    return fd;
}

void setStats(ImageData*data, long size, long mod_time) {
    data->size = size;
    data->mod_time = mod_time;
    data->stats_loaded = 1;
}

void loadStats(ImageData*data) {
    if(data->stats_loaded)
        return;
    struct stat statbuf;
    int fd = getFD(data);
    if(!fstat(fd, &statbuf)) {
        setStats(data, statbuf.st_size, statbuf.st_mtim.tv_sec);
    }
    data->stats_loaded = 1;
}

int compareName(const void* a, const void* b) {
    return strcmp((*(ImageData**)a)->name, (*(ImageData**)b)->name);
}

int compareMod(const void* a, const void* b) {
    return (*(ImageData**)a)->mod_time - (*(ImageData**)b)->mod_time;
}

int compareSize(const void* a, const void* b) {
    return (*(ImageData**)a)->size - (*(ImageData**)b)->size;
}

int compareId(const void* a, const void* b) {
    return (*(ImageData**)a)->id - (*(ImageData**)b)->id;
}

void sortImages(ImageContext* context, int type) {
    static int (*sort_func[])(const void*, const void* b) = {
        [IMG_SORT_LOADED] = compareId,
        [IMG_SORT_NAME] = compareName,
        [IMG_SORT_MOD] = compareMod,
        [IMG_SORT_SIZE] = compareSize,
    };
    if(!(context->flags & LOAD_STATS) && abs(type) > IMG_SORT_NAME)
        for(int i = 0;i < context->num; i++) {
            loadStats(context->data[i]);
        }
    qsort(context->data, context->num, sizeof(context->data[0]), sort_func[abs(type)]);

    if(type < 0) {
        for(int i = 0;i < context->num/2; i++) {
            void* temp = context->data[i];
            context->data[i] = context->data[context->num -1 - i];
            context->data[context->num -1 - i] = temp;
        }
    }
}

void removeInvalid(ImageContext* context) {
    int i = 0;
    for(i = 0;i < context->num && context->data[i]; i++);
    for(int j = 1; i + j < context->num; i++)
        context->data[i] = context->data[i + j];
    context->num = i;
}

void closeImage(ImageData* data, int force) {
    if(--data->ref_count == 0 &&  !(data->flags & IMG_DATA_KEEP_OPEN)|| force) {
        data->loader->img_close(data);
        data->image_data = NULL;
        data->data = NULL;
    }
}

void freeImageData(ImageContext*context, ImageData* data) {
    if(data->data)
        closeImage(data, 1);

    if(data->flags & IMG_DATA_FREE_NAME)
        free((void*)data->name);

    free(data);
    for(int i = context->num - 1; i >= 0; i--)
        if (context->data[i] == data)
            context->data[i] = NULL;
}

void destroyContext(ImageContext*context) {
    LOG("Destroy context\n");
    for(int i = context->num - 1; i >= 0; i--){
        freeImageData(context, context->data[i]);
    }
    free(context->data);
    free(context);
}

int loadImageWithLoader(ImageContext* context, int fd, ImageData*data, const ImageLoader* img_loader) {
    int ret = img_loader->img_open(context, fd, data);
    LOG("Loader %s returned %d\n", img_loader->name, ret);
    if (ret == 0) {
        data->loader = img_loader;
        if(data->flags & IMG_DATA_FLIP_RED_BLUE)
            flipRedBlue(data);
    }
    return ret;
}

ImageData* _loadImage(ImageContext* context, ImageData*data, int multi_lib_only) {
    int fd = getFD(data);
    for(int i = 0; i < sizeof(img_loaders)/sizeof(img_loaders[0]); i++) {
        if(multi_lib_only && (img_loaders[i].flags & MULTI_LOADER))
            continue;
        if(fd == -1 && !(img_loaders[i].flags & NO_FD))
            continue;
        if(loadImageWithLoader(context, fd, data, &img_loaders[i]) == 0)
            return data;
        if(!(img_loaders[i].flags & NO_SEEK))
            lseek(fd, 0, SEEK_SET);
    }
    if(fd != -1)
        close(fd);
    return NULL;
}

ImageData* loadImage(ImageContext* context, ImageData*data) {
    if(data->data || _loadImage(context, data, 0)) {
        data->ref_count++;
        return data;
    }
    return NULL;
}

struct ImageData* openImage(struct ImageContext* context, int index, struct ImageData* currentImage) {
    ImageData* data = NULL;
    if( index >= 0  && index < context->num) {
        if(currentImage == context->data[index])
            return currentImage;
        int num = context->num;
        data = loadImage(context, context->data[index]);
        int remove = 0;
        int diff = context->num - num;
        if((!data || !data->data) && context->flags & REMOVE_INVALID) {
            freeImageData(context, context->data[index]);
            context->data[index] = NULL;
            removeInvalid(context);
            return openImage(context, index, currentImage);
        }
    }
    if(currentImage) {
        closeImage(currentImage, 0);
    }
    return data;
}

ImageData* addFile(ImageContext* context, const char* file_name) {
    LOG("Attempting to add file %s\n", file_name);
    if(context->num == context->size || !context->data) {
        if(context->data)
            context->size *= 2;
        context->data = realloc(context->data, context->size * sizeof(context->data[0]));
    }
    ImageData* data = calloc(1, sizeof(ImageData));
    data->fd = -1;
    context->data[context->num] = data ;
    data->id = context->counter++;
    data->name = file_name;
    if(context->flags & LOAD_STATS)
        loadStats(context->data[context->num]);
    if(context->flags & PRE_EXPAND) {
        if(_loadImage(context, context->data[context->num], 1));
    }
    context->num++;
    LOG("Added file %s %d\n", file_name, context->num);
    return data;
}

int addFromPipe(ImageContext* context, int fd, const char* name) {
    ImageData* data = addFile(context, name);
    return loadImageWithLoader(context, fd, data, &pipe_loader);
}

ImageContext* createContext(const char** file_names, int num, int flags) {
    ImageContext* context = calloc(1, sizeof(ImageContext));
    context->flags = flags;
    context->size = num ? num : 16;
    for(int i = 0; (!num || i < num) && file_names && file_names[i]; i++) {
        if(file_names[i][0] == '-' && !file_names[i][1])
            addFromPipe(context, STDIN_FILENO, "stdin");
        else
            addFile(context, file_names[i]);
    }
    return context;
}

const char* getImageName(const ImageData* data){return data->name;}
unsigned int getImageNum(const ImageContext* context){return context->num;}
unsigned int getImageWidth(const ImageData* data){return data->image_width;};
unsigned int getImageHeight(const ImageData* data){return data->image_height;};;
void* getRawImage(const ImageData* data) { return data->data;}

int createMemoryFile(const char* name, int size) {
    int fd = memfd_create(name, MFD_CLOEXEC);
    if(size)
        ftruncate(fd, size);
    return fd;
}


void flipRedBlue(ImageData* data) {
    char* raw = data->data;
    for(int i = 0; i < data->image_width * data->image_height *4; i+=4) {
        char temp = raw[i];
        raw[i] = raw[i+2];
        raw[i+2] = temp;
    }
}
