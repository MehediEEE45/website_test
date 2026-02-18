# Railway Deployment Guide

## Quick Start (1-5 minutes)

### 1. Connect Your GitHub Repository
1. Go to [railway.app](https://railway.app)
2. Click **"New Project"** → **"Deploy from GitHub repo"**
3. Authorize GitHub and select `website_test` repository
4. Railway will auto-detect Node.js and start building

### 2. Configure Environment Variables
In the Railway dashboard, go to **Variables** tab and add:

**MQTT Configuration:**
```
MQTT_URL=wss://0d34f5789e1e4a669367abfe5bd45b15.s1.eu.hivemq.cloud:8884
MQTT_USERNAME=battery
MQTT_PASSWORD=your_password
MQTT_TOPIC_FILTER=energy/+/+/telemetry
MQTT_TOPIC_FILTER_2=smartpower/+/data
MQTT_TOPIC_FILTER_3=battery/data
```

**MongoDB Configuration:**
```
MONGO_URI=mongodb+srv://username:password@your-cluster.mongodb.net/battery_monitor?retryWrites=true&w=majority
MONGO_DB=battery_monitor
MONGO_COLLECTION=telemetry
MONGO_TTL_DAYS=30
```

**Server Configuration:**
```
PORT=3000
DB_FILE=telemetry.db
```

### 3. Deploy
Railway will automatically deploy:
- ✅ Node.js server with API endpoints
- ✅ Static website from `/public` directory
- ✅ MongoDB bridge for data persistence
- ✅ MQTT client for sensor data

### 4. Access Your Live Site
Your website will be available at: **`https://your-railway-project.up.railway.app`**

Railway automatically assigns a domain and provides SSL/TLS.

## Features Deployed

✅ **Real-time Dashboard**: Live MQTT battery metrics  
✅ **30-Day Analytics**: Historical data from MongoDB  
✅ **Data Export**: CSV and JSON download  
✅ **API Endpoints**: REST API for data queries  
✅ **WebSocket Bridge**: Live updates to connected clients  

## Automatic Updates

Every push to `main` branch will:
1. Trigger GitHub Actions → build & push
2. Railway monitors your repo
3. Auto-redeploys your changes within 2-3 minutes

## Monitoring

In Railway dashboard you can:
- View logs in real-time
- Check deployment status
- Monitor resource usage
- Set domain aliases

## Troubleshooting

**Server not starting?**
- Check logs: Railway dashboard → Deployments → Logs
- Verify all environment variables are set
- Ensure MongoDB URI is correct

**Data not appearing?**
- Check MQTT_URL and credentials
- Verify MongoDB connection in logs
- Ensure ESP32 is publishing data

**Website routes not working?**
- Server now serves static files from `/public`
- API routes work at `/api/*`
- Root `/` serves `index.html`

## Free Tier Limits

Railway free tier includes:
- Up to $5/month credit
- Sufficient for hobby projects
- No credit card required for trial
