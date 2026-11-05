def fit(ndotv, alpha, F0, F90):
    x=ndotv; y=alpha; x2=x*x; y2=y*y
    c=[(0.1003,0.9345,1.0,1.0),(-0.6303,-2.323,-1.765,0.2281),(9.748,2.229,8.263,15.94),(-2.038,-3.748,11.53,-55.83),
       (29.34,1.424,28.96,13.08),(-8.245,-0.7684,-7.507,41.26),(-26.44,1.436,-36.11,54.9),(19.99,0.2913,15.86,300.2),(-5.448,0.6286,33.37,-285.1)]
    t=[1,x,y,x*y,x2,y2,x2*y,x*y2,x2*y2]
    r=[max(0.0,sum(c[k][i]*t[k] for k in range(9))) for i in range(4)]
    A=r[0]/r[2]; B=r[1]/r[3]
    a=max(0.0,1-ndotv); p5=a**5; p2=(1-min(max(alpha*20,0),1))**2
    v=(F0*A+F90*B)*(1-p2)+(F0+(F90-F0)*p5)*p2
    return min(max(v,0),1), A, B
def smooth(a,b,x):
    t=min(max((x-a)/(b-a),0),1); return t*t*(3-2*t)
def fit_ior(ndotv, alpha, ior):
    f0=((1-ior)/(1+ior))**2
    return fit(ndotv,alpha,f0,1.0)[0]**(smooth(1.0,1.4,ior)**-0.2)
for a,Ess in [(0.09,0.99067),(0.64,0.55500)]:
    alb=fit_ior(1.0,a,1.5); e11=fit(1.0,a,1,1)[0]
    print("alpha",a,"alb_fit_ior",round(alb,6),"fitE11",round(e11,6),"alb*Ess/fitE11",round(alb*Ess/e11,6))
