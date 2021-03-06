#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static int cnt_call=1;
struct drm_object{
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	uint32_t id;
};

struct modeset_buf{
	uint32_t width;
	uint32_t height;
	uint32_t size;
	uint32_t stride;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};

struct modeset_device{
	struct modeset_device *next;
	unsigned int front_buf;
	struct modeset_buf bufs[2];

	struct drm_object connector;
	struct drm_object crtc;
	struct drm_object plane;

	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	uint32_t crtc_index;

	bool pflip_pending;
	bool cleanup;

	uint8_t r,g,b;
	bool r_up,g_up,b_up;
};

static struct modeset_device *device_list=NULL;

static int modeset_open(int *out,const char *node){
	int fd,ret;
	uint64_t cap;

	fd=open(node,O_RDWR|O_CLOEXEC);
	if(fd<0){
		ret=-errno;
		fprintf(stderr,"cannot open '%s' : %m\n",node);
		return ret;
	}

	ret=drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1);
	if(ret){
		fprintf(stderr,"failed to set universal planes cap,%d\n",ret);
		return ret;
	}

	ret=drmSetClientCap(fd,DRM_CLIENT_CAP_ATOMIC,1);
	if(ret){
		fprintf(stderr,"failed to set atomic cap,%d\n",ret);
		return ret;
	}

	if(drmGetCap(fd,DRM_CAP_DUMB_BUFFER,&cap)<0||!cap){
		fprintf(stderr,"drm device '%s' does not support dumb buffers \n",node);
		close(fd);
		return -ENOTSUP;
	}

	if(drmGetCap(fd,DRM_CAP_CRTC_IN_VBLANK_EVENT,&cap)<0||!cap){
		fprintf(stderr,"drm device '%s' does not support atomic KMS \n",node);
		close(fd);
		return -ENOTSUP;
	}

	*out=fd;
	return 0;

}

static int64_t get_property_value(int fd,drmModeObjectPropertiesPtr props,const char *name){
	drmModePropertyPtr prop;
	uint64_t value;
	bool found;
	int j;

	found = false;

	for(j=0;j<props->count_props&&!found;j++){
		prop=drmModeGetProperty(fd,props->props[j]);
		if(!strcmp(prop->name,name)){
			value=props->prop_values[j];
			found=true;
		}
		drmModeFreeProperty(prop);
	}

	if(!found)
		return -1;
	return value;
}

static void modeset_get_object_properties(int fd,struct drm_object *obj,uint32_t type){
	const char *type_str;
	unsigned int i;

	obj->props=drmModeObjectGetProperties(fd,obj->id,type);
	if(!obj->props){
		switch(type){
			case DRM_MODE_OBJECT_CONNECTOR:
				type_str="connector";
				break;
			case DRM_MODE_OBJECT_PLANE:
				type_str="plane";
				break;
			case DRM_MODE_OBJECT_CRTC:
				type_str="crtc";
				break;
			default:
				type_str="unknown type";
				break;
		}

		fprintf(stderr,"cannot get %s %d properties :%s \n",type_str,obj->id,strerror(errno));
		return;
	}

	obj->props_info=calloc(obj->props->count_props,sizeof(obj->props_info));
	for(i=0;i<obj->props->count_props;i++){
		obj->props_info[i]=drmModeGetProperty(fd,obj->props->props[i]);
	}

}

static int set_drm_object_property(drmModeAtomicReq *req,struct drm_object *obj,const char *name,uint64_t value){
	int i;
	uint32_t prop_id=0;

	for(i=0;i<obj->props->count_props;i++){
		if(!strcmp(obj->props_info[i]->name,name)){
			prop_id=obj->props_info[i]->prop_id;
			break;
		}
	}

	if(prop_id==0){
		fprintf(stderr,"no object propert :%s\n",name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req,obj->id,prop_id,value);
}

static int modeset_find_crtc(int fd,drmModeRes *res,drmModeConnector *conn,struct modeset_device *dev){
	drmModeEncoder *enc;
	unsigned int i,j;
	uint32_t crtc;
	struct modeset_device *iter;

	if(conn->encoder_id)
		enc=drmModeGetEncoder(fd,conn->encoder_id);
	else
		enc=NULL;

	if(enc){
		if(enc->crtc_id){
			crtc=enc->crtc_id;
			for(iter=device_list;iter;iter=iter->next){
				if(iter->crtc.id==crtc){
					crtc=0;
					break;
				}
			}

			if(crtc>0){
				drmModeFreeEncoder(enc);
				dev->crtc.id=crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}
				
	for(i=0;i<conn->count_encoders;++i){
		enc=drmModeGetEncoder(fd,conn->encoders[i]);
		if(!enc){
			fprintf(stderr,"cannot retrieve encoder %u:%u (%d):%m\n",i,conn->encoders[i],errno);
			continue;
		}

		
		for(j=0;j<res->count_crtcs;++j){
			if(!(enc->possible_crtcs&(1<<j)))
				continue;

		crtc=res->crtcs[j];
		for(iter=device_list;iter;iter=iter->next){
				if(iter->crtc.id==crtc){
					crtc=0;
					break;
				}
			}

			if(crtc>0){
				fprintf(stdout,"crtc %u found for encoder %u ,will need a full modeset \n",crtc,conn->encoders[i]);
				drmModeFreeEncoder(enc);
				dev->crtc.id=crtc;
				dev->crtc_index=j;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr,"cannot find suitable crtc for connector %u \n",conn->connector_id);
	return -ENOENT;
}

static int modeset_find_plane(int fd, struct modeset_device *dev){
	drmModePlaneResPtr plane_res;
	bool found_primary=false;
	int i,ret=-EINVAL;

	plane_res=drmModeGetPlaneResources(fd);
	if(!plane_res){
		fprintf(stderr,"drmModeGetPlaneResources Failed:%s\n",strerror(errno));
		return -ENOENT;
	}

	for(i=0;(i<plane_res->count_planes)&&!found_primary;i++){
		int plane_id=plane_res->planes[i];	

		drmModePlanePtr plane=drmModeGetPlane(fd,plane_id);
		if(!plane){
			fprintf(stderr,"drmModeGetPlane(%u) failed :%s \n",plane_id,strerror(errno));
			continue;
		}

		if(plane->possible_crtcs&(1<<dev->crtc_index)){
			drmModeObjectPropertiesPtr props=drmModeObjectGetProperties(fd,plane_id,DRM_MODE_OBJECT_PLANE);
			if(get_property_value(fd,props,"type")==DRM_PLANE_TYPE_PRIMARY){
				found_primary=true;
				dev->plane.id=plane_id;
				ret=0;
			}

			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);

	if(found_primary)
		fprintf(stdout,"found primary plane ,id : %d\n",dev->plane.id);
	else
		fprintf(stdout,"couldn't find primary plane\n");
	return ret;
}

static void modeset_drm_object_finish(struct drm_object *obj){
	int i;
	for(i=0;i<obj->props->count_props;i++)
		drmModeFreeProperty(obj->props_info[i]);
	free(obj->props_info);
	drmModeFreeObjectProperties(obj->props);
}
	
static int modeset_setup_objects(int fd,struct modeset_device *dev){
	struct drm_object *connector=&dev->connector;
	struct drm_object *crtc=&dev->crtc;
	struct drm_object *plane=&dev->plane;

	modeset_get_object_properties(fd,connector,DRM_MODE_OBJECT_CONNECTOR);
	if(!connector->props)
		goto out_conn;
	
	modeset_get_object_properties(fd,crtc,DRM_MODE_OBJECT_CRTC);
	if(!crtc->props)
		goto out_crtc;

	modeset_get_object_properties(fd,plane,DRM_MODE_OBJECT_PLANE);
	if(!plane->props)
		goto out_plane;



	return 0;

out_plane:
	modeset_drm_object_finish(crtc);
out_crtc:
	modeset_drm_object_finish(connector);
out_conn:
	return -ENOMEM;
}

static void modeset_destroy_objects(int fd,struct modeset_device *dev){
	modeset_drm_object_finish(&dev->connector);
	modeset_drm_object_finish(&dev->crtc);
	modeset_drm_object_finish(&dev->plane);
}

static int modeset_create_fb(int fd,struct modeset_buf *buf){
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;
	uint32_t handles[4]={0},pitches[4]={0},offsets[4]={0};

	memset(&creq,0,sizeof(creq));
	creq.width=buf->width;
	creq.height=buf->height;
	creq.bpp=32;
	ret=drmIoctl(fd,DRM_IOCTL_MODE_CREATE_DUMB,&creq);
	if(ret<0){
		fprintf(stderr,"cannot create dumb buffer (%d):%m\n",errno);
		return -errno;
	}
	buf->stride=creq.pitch;
	buf->handle=creq.handle;
	buf->size=creq.size;

	handles[0]=buf->handle;
	pitches[0]=buf->stride;
	
	ret=drmModeAddFB2(fd,buf->width,buf->height,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&buf->fb,0);
	
	if(ret){
		fprintf(stderr,"cannot create framebuffer (%d):%m\n",errno);
		ret=-errno;
		goto err_destroy;
	}

	memset(&mreq,0,sizeof(mreq));
	mreq.handle=buf->handle;
	ret=drmIoctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&mreq);
	if(ret){
		fprintf(stderr,"cannot prepare framebuffer for mapping (%d):%m\n",errno);
		ret=-errno;
		goto err_fb;
	}

	buf->map=mmap(0,buf->size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,mreq.offset);
	if(buf->map==MAP_FAILED){
		fprintf(stderr,"cannot mmap dumb buffer (%d):%m\n",errno);
		ret=-errno;
		goto err_fb;
	}
	
	memset(buf->map,0,buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd,buf->fb);
err_destroy:
	memset(&dreq,0,sizeof(dreq));
	dreq.handle=buf->handle;
	drmIoctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&dreq);
	return ret;
}

static void modeset_destroy_fb(int fd,struct modeset_buf *buf){
	struct drm_mode_destroy_dumb dreq;

	munmap(buf->map,buf->size);
	drmModeRmFB(fd,buf->fb);
	memset(&dreq,0,sizeof(dreq));
	dreq.handle=buf->handle;
	drmIoctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&dreq);
}

static int modeset_setup_framebuffer(int fd,drmModeConnector *conn,struct modeset_device *dev){
	int i,ret;

	for(i=0;i<2;i++){
		dev->bufs[i].width=conn->modes[0].hdisplay;
		dev->bufs[i].height=conn->modes[0].vdisplay;

		ret=modeset_create_fb(fd,&dev->bufs[i]);
		if(ret){
			if(i==1){
				modeset_destroy_fb(fd,&dev->bufs[0]);
			}
			return ret;
		}
	}

	return 0;
}

static void modeset_device_destory(int fd,struct modeset_device *dev){
	modeset_destroy_objects(fd,dev);

	modeset_destroy_fb(fd,&dev->bufs[0]);
	modeset_destroy_fb(fd,&dev->bufs[1]);

	drmModeDestroyPropertyBlob(fd,dev->mode_blob_id);

	free(dev);
}

static struct modeset_device *modeset_device_create(int fd,drmModeRes *res,drmModeConnector *conn){
	int ret;
	struct modeset_device *dev;

	dev=malloc(sizeof(*dev));
	memset(dev,0,sizeof(*dev));
	dev->connector.id=conn->connector_id;

	if(conn->connection!=DRM_MODE_CONNECTED){
		fprintf(stderr,"ignoring unused connector %u\n",conn->connector_id);
		goto dev_error;
	}

	if(conn->count_modes==0){
		fprintf(stderr,"no valid mode for connector %u\n",conn->connector_id);
		goto dev_error;
	}

	memcpy(&dev->mode,&conn->modes[0],sizeof(dev->mode));
	if(drmModeCreatePropertyBlob(fd,&dev->mode,sizeof(dev->mode),&dev->mode_blob_id)!=0){
		fprintf(stderr,"couldn't create a blob property\n");
		goto dev_error;
	}


	ret=modeset_find_crtc(fd,res,conn,dev);
	if(ret){
		fprintf(stderr,"no valid crtc for connector %u\n",conn->connector_id);
		goto dev_blob;
	}

	ret=modeset_find_plane(fd,dev);
	if(ret){
		fprintf(stderr,"no valid plane for crtc %u\n",dev->crtc.id);
		goto dev_blob;
	}

	ret=modeset_setup_objects(fd,dev);
	if(ret){
		fprintf(stderr,"cannnot get properties \n");
		goto dev_blob;
	}

	ret=modeset_setup_framebuffer(fd,conn,dev);
	if(ret){
		fprintf(stderr,"connot create framebuffers for connector %u\n",conn->connector_id);
		goto dev_blob;
	}

	fprintf(stderr,"mode for connector %u is %ux%u\n",conn->connector_id,dev->bufs[0].width,dev->bufs[0].height);
	return dev;

dev_obj:
	modeset_destroy_objects(fd,dev);
dev_blob:
	drmModeDestroyPropertyBlob(fd,dev->mode_blob_id);
dev_error:
	free(dev);
	return NULL;
}

static int modeset_prepare(int fd){
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_device *dev;

	res=drmModeGetResources(fd);
	if(!res){
		fprintf(stderr,"cannot retrieve DRM resources (%d):%m\n",errno);
		return -errno;
	}

	for(i=0;i<res->count_connectors;++i){
		conn=drmModeGetConnector(fd,res->connectors[i]);
		if(!conn){
			fprintf(stderr,"cannot retrieve DRM connector %u:%u (%d):%m\n",i,res->connectors[i],errno);
			continue;
		}

		dev=modeset_device_create(fd,res,conn);
		drmModeFreeConnector(conn);
		if(!dev)
			continue;

		dev->next=device_list;
		device_list=dev;
	}
	if(!device_list){
		fprintf(stderr,"couldn't create any devices\n");
		return -1;
	}

	drmModeFreeResources(res);
	return 0;
}

static int modeset_atomic_prepare_commit(int fd,struct modeset_device *dev,drmModeAtomicReq *req){
	struct drm_object *plane=&dev->plane;
	struct modeset_buf *buf=&dev->bufs[dev->front_buf^1];

	if(set_drm_object_property(req,&dev->connector,"CRTC_ID",dev->crtc.id)<0)
		return -1;
	
	if(set_drm_object_property(req,&dev->crtc,"MODE_ID",dev->mode_blob_id)<0)
		return -1;
		
	if(set_drm_object_property(req,&dev->crtc,"ACTIVE",1)<0)
		return -1;

	if(set_drm_object_property(req,plane,"FB_ID",buf->fb)<0)
		return -1;

	if(set_drm_object_property(req,plane,"CRTC_ID",dev->crtc.id)<0)
		return -1;

	if(set_drm_object_property(req,plane,"SRC_X",0)<0)
		return -1;

	if(set_drm_object_property(req,plane,"SRC_Y",0)<0)
		return -1;

	if(set_drm_object_property(req,plane,"SRC_W",buf->width<<16)<0)
		return -1;

	if(set_drm_object_property(req,plane,"SRC_H",buf->height<<16)<0)
		return -1;

	if(set_drm_object_property(req,plane,"CRTC_X",0)<0)
		return -1;

	if(set_drm_object_property(req,plane,"CRTC_Y",0)<0)
		return -1;

	if(set_drm_object_property(req,plane,"CRTC_W",buf->width)<0)
		return -1;

	if(set_drm_object_property(req,plane,"CRTC_H",buf->height)<0)
		return -1;

	return 0;
}

static uint8_t next_color(bool *up,uint8_t cur,unsigned int mod){
	uint8_t next;

	next=cur+(*up?1:-1)*(rand()%mod);
	if((*up&&next<cur)||(!*up&&next>cur)){
		*up=!*up;
		next=cur;
	}

	return next;

}

static void modeset_draw_framebuffer(struct modeset_device *dev){
	struct modeset_buf *buf;
	unsigned int j,k,off,random;
	char time_left[5];
	cairo_t* cr;
	cairo_surface_t *surface,*image,*image2,*image3,*image4,*image5,*image6,*image7,*image8,*image9,*image10;

	cairo_text_extents_t te;
	/*
	dev->r=next_color(&dev->r_up,dev->r,5);
	dev->g=next_color(&dev->g_up,dev->g,5);
	dev->b=next_color(&dev->b_up,dev->b,5);
	*/
	buf=&dev->bufs[dev->front_buf^1];
	for(j=0;j<buf->height;++j){
		for(k=0;k<buf->width;++k){
			off=buf->stride*j+k*4;
			*(uint32_t*)&buf->map[off]=(0<<16)|(0<<8)|0;
		}
	}
	image = cairo_image_surface_create_from_png("21600.png");
	image2 = cairo_image_surface_create_from_png("21602.png");
	image3 = cairo_image_surface_create_from_png("21603.png");
	image4 = cairo_image_surface_create_from_png("21604.png");
	image5 = cairo_image_surface_create_from_png("21601.png");
	image6 = cairo_image_surface_create_from_png("21606.png");
	image7 = cairo_image_surface_create_from_png("21607.png");
	image8 = cairo_image_surface_create_from_png("21608.png");
	image9 = cairo_image_surface_create_from_png("21609.png");
	image10= cairo_image_surface_create_from_png("21610.png");
	surface = cairo_image_surface_create_for_data (buf->map, CAIRO_FORMAT_ARGB32,
		buf->width, buf->height,buf->stride);
	cr = cairo_create (surface);
	cairo_select_font_face (cr, "Georgia",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_source_rgb (cr, 255.0, 255.0, 255.0);
	cairo_set_font_size (cr,100);
	if(cnt_call<=10){
	//	itoa(cnt_call,time_left,10);
	sprintf(time_left, "%d",10- cnt_call);
	
	cairo_text_extents (cr, "a", &te);
	cairo_move_to (cr,350,buf->height/2); 
	cairo_show_text (cr, "Starting Slide Show in ");
	cairo_text_extents (cr, "a", &te);
	cairo_move_to (cr,buf->width/2,buf->height/2+150); 
	cairo_show_text (cr, time_left );
	cnt_call++;}
	
	else{
	/*cairo_set_line_width (cr, 0.1);
	cairo_set_source_rgb (cr, 255, 255, 255);
	cairo_rectangle (cr, 100,100, 100.5, 100.5);
	//cairo_move_to (cr, 100, 100);
	//cairo_line_to (cr, 300, 300);
	cairo_stroke (cr);8=*/
	random=rand()%10;
	if(random==0)
		cairo_set_source_surface(cr, image, 0, 0);
	else if(random==1)
		cairo_set_source_surface(cr, image2, 0, 0);
	else if(random==2)
		cairo_set_source_surface(cr, image3, 0, 0);
	else if(random==3)
		cairo_set_source_surface(cr, image4, 0, 0);
	else if(random==4)
		cairo_set_source_surface(cr, image6, 0, 0);
	else if(random==5)
		cairo_set_source_surface(cr, image7, 0, 0);
	else if(random==6)
		cairo_set_source_surface(cr, image8, 0, 0);
	else if(random==7)
		cairo_set_source_surface(cr, image9, 0, 0);
	else if(random==8)
		cairo_set_source_surface(cr, image10, 0, 0);
	else
		cairo_set_source_surface(cr, image5, 0, 0);

  	cairo_paint(cr);
	}
  usleep(700000);

}

static void modeset_draw_output(int fd,struct modeset_device *dev){
	drmModeAtomicReq *req;
	int ret,flags;

	modeset_draw_framebuffer(dev);
	req=drmModeAtomicAlloc();
	ret=modeset_atomic_prepare_commit(fd,dev,req);
	if(ret<0){
		fprintf(stderr,"prepare atomic commit failed, %d \n",errno);
		return;
	}

	flags=DRM_MODE_PAGE_FLIP_EVENT|DRM_MODE_ATOMIC_NONBLOCK;
	ret=drmModeAtomicCommit(fd,req,flags,NULL);
	drmModeAtomicFree(req);

	if(ret<0){
		fprintf(stderr,"atomic commit failed ,%d\n",errno);
		return;
	}

	dev->front_buf^=1;
	dev->pflip_pending=true;
}

static void modeset_page_flip_event(int fd,unsigned int frame,unsigned int sec,unsigned int usec,unsigned int crtc_id,void *data){
	struct modeset_device *dev,*iter;

	dev=NULL;
	for(iter=device_list;iter;iter=iter->next){
		if(iter->crtc.id==crtc_id){
			dev=iter;
			break;
		}
	}

	if(dev==NULL)
		return;

	dev->pflip_pending=false;
	if(!dev->cleanup)
		modeset_draw_output(fd,dev);
}

static int modeset_perform_modeset(int fd){
	int ret,flags;
	struct modeset_device *iter;
	drmModeAtomicReq *req;

	req=drmModeAtomicAlloc();
	for(iter=device_list;iter;iter=iter->next){
		ret=modeset_atomic_prepare_commit(fd,iter,req);
		if(ret<0)
			break;
	}

	if(ret<0){
		fprintf(stderr,"prepare atomic commit failed,%d\n",errno);
		return ret;
	}

	flags=DRM_MODE_ATOMIC_TEST_ONLY|DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret=drmModeAtomicCommit(fd,req,flags,NULL);
	if(ret<0){
		fprintf(stderr,"test-only atomic commit failed,%d\n",errno);
		drmModeAtomicFree(req);
		return ret;
	}

	for(iter=device_list;iter;iter=iter->next){
		iter->r=rand()%0xff;
		iter->g=rand()%0xff;
		iter->b=rand()%0xff;
		iter->r_up=iter->g_up=iter->b_up=true;

		modeset_draw_framebuffer(iter);
	}

	flags=DRM_MODE_PAGE_FLIP_EVENT|DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret=drmModeAtomicCommit(fd,req,flags,NULL);
	if(ret<0)
		fprintf(stderr,"test-only atomic commit failed,%d\n",errno);
	
	drmModeAtomicFree(req);

	return ret;
}

static void modeset_draw(int fd)
{
	int ret;
	fd_set fds;
	time_t start, cur;
	struct timeval v;
	drmEventContext ev;

	srand(time(&start));
	FD_ZERO(&fds);
	memset(&v, 0, sizeof(v));
	memset(&ev, 0, sizeof(ev));

	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	modeset_perform_modeset(fd);

	while (time(&cur) < start + 45) {
		FD_SET(0, &fds);
		FD_SET(fd, &fds);
		v.tv_sec = start + 45 - cur;

		ret = select(fd + 1, &fds, NULL, NULL, &v);
		if (ret < 0) {
			fprintf(stderr, "select() failed with %d: %m\n", errno);
			break;
		} else if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			break;
		} else if (FD_ISSET(fd, &fds)) {
			drmHandleEvent(fd, &ev);
		}
	}
}

static void modeset_cleanup(int fd)
{
	struct modeset_device *iter;
	drmEventContext ev;
	int ret;

	memset(&ev, 0, sizeof(ev));
	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	while (device_list) {
		iter = device_list;

		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			ret = drmHandleEvent(fd, &ev);
			if (ret)
				break;
		}

		device_list = iter->next;

		modeset_device_destory(fd, iter);
	}
}


int main(int argc, char **argv)
{
	int ret, fd;
	const char *card;

	if (argc > 1)
		card = argv[1];
	else
		card = "/dev/dri/card0";

	fprintf(stderr, "using card '%s'\n", card);

	ret = modeset_open(&fd, card);
	if (ret)
		goto out_return;

	ret = modeset_prepare(fd);
	if (ret)
		goto out_close;

	modeset_draw(fd);

	modeset_cleanup(fd);

	ret = 0;

out_close:
	close(fd);
out_return:
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modeset failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "exiting\n");
	}
	return ret;
}

