/**
 * Energy Monitoring Dashboard - Authentication & Access Control System
 * This is a client-side demo. In production, use Supabase/Firebase/custom backend.
 */

const AUTH = {
    // Permission definitions
    PERMISSIONS: {
        SOLAR_VIEW: 'solar.view',
        SOLAR_MANAGE: 'solar.manage',
        WIND_VIEW: 'wind.view',
        WIND_MANAGE: 'wind.manage',
        BATTERY_VIEW: 'battery.view',
        BATTERY_MANAGE: 'battery.manage',
        CONTROL_PANEL: 'controlpanel.manage',
        ADMIN: 'system.admin'
    },

    // Default groups with permissions
    DEFAULT_GROUPS: [
        {
            id: 'grp_admin',
            name: 'Administrators',
            description: 'Full system access',
            permissions: ['solar.view', 'solar.manage', 'wind.view', 'wind.manage', 'battery.view', 'battery.manage', 'controlpanel.manage', 'system.admin'],
            color: '#ef4444'
        },
        {
            id: 'grp_solar',
            name: 'Solar Team',
            description: 'Access to solar monitoring only',
            permissions: ['solar.view', 'solar.manage'],
            color: '#f59e0b'
        },
        {
            id: 'grp_wind',
            name: 'Wind Team',
            description: 'Access to wind monitoring only',
            permissions: ['wind.view', 'wind.manage'],
            color: '#06b6d4'
        },
        {
            id: 'grp_battery',
            name: 'Battery Team',
            description: 'Access to battery monitoring only',
            permissions: ['battery.view', 'battery.manage'],
            color: '#8b5cf6'
        },
        {
            id: 'grp_operators',
            name: 'Operators',
            description: 'View access to all systems',
            permissions: ['solar.view', 'wind.view', 'battery.view'],
            color: '#10b981'
        },
        {
            id: 'grp_full',
            name: 'Full Access',
            description: 'Access to all monitoring systems',
            permissions: ['solar.view', 'solar.manage', 'wind.view', 'wind.manage', 'battery.view', 'battery.manage', 'controlpanel.manage'],
            color: '#3b82f6'
        }
    ],

    // Default admin user
    DEFAULT_ADMIN: {
        id: 'usr_admin',
        email: 'admin@energy.local',
        name: 'System Admin',
        password: 'admin123', // In production: hashed!
        groups: ['grp_admin'],
        status: 'active',
        createdAt: new Date().toISOString()
    },

    // Initialize the auth system
    init() {
        if (!localStorage.getItem('auth_groups')) {
            localStorage.setItem('auth_groups', JSON.stringify(this.DEFAULT_GROUPS));
        }
        if (!localStorage.getItem('auth_users')) {
            localStorage.setItem('auth_users', JSON.stringify([this.DEFAULT_ADMIN]));
        }
        if (!localStorage.getItem('auth_audit')) {
            localStorage.setItem('auth_audit', JSON.stringify([]));
        }
    },

    // Get all groups
    getGroups() {
        return JSON.parse(localStorage.getItem('auth_groups') || '[]');
    },

    // Save groups
    saveGroups(groups) {
        localStorage.setItem('auth_groups', JSON.stringify(groups));
    },

    // Get all users
    getUsers() {
        return JSON.parse(localStorage.getItem('auth_users') || '[]');
    },

    // Save users
    saveUsers(users) {
        localStorage.setItem('auth_users', JSON.stringify(users));
    },

    // Get current logged in user
    getCurrentUser() {
        const session = localStorage.getItem('auth_session');
        if (!session) return null;
        try {
            const { userId, expiresAt } = JSON.parse(session);
            if (new Date(expiresAt) < new Date()) {
                this.logout();
                return null;
            }
            const users = this.getUsers();
            return users.find(u => u.id === userId) || null;
        } catch {
            return null;
        }
    },

    // Get user object by id or return same user object
    getUserById(userOrId) {
        if (!userOrId) return null;
        if (typeof userOrId === 'string') {
            const users = this.getUsers();
            return users.find(u => u.id === userOrId) || null;
        }
        // assume object
        return userOrId && userOrId.id ? userOrId : null;
    },

    // Get user's groups as objects (accepts user object or userId)
    getUserGroups(userOrId) {
        const user = this.getUserById(userOrId);
        if (!user) return [];
        const groups = this.getGroups();
        return (user.groups || []).map(gid => groups.find(g => g.id === gid)).filter(Boolean);
    },

    // Get user's permissions (from all their groups). Accepts user object or userId
    getUserPermissions(userOrId) {
        const user = this.getUserById(userOrId);
        if (!user) return [];
        const groups = this.getGroups();
        const permissions = new Set();
        (user.groups || []).forEach(groupId => {
            const group = groups.find(g => g.id === groupId);
            if (group) {
                group.permissions.forEach(p => permissions.add(p));
            }
        });
        return Array.from(permissions);
    },

    // Check if user has specific permission
    hasPermission(permission) {
        const user = this.getCurrentUser();
        if (!user) return false;
        const permissions = this.getUserPermissions(user);
        return permissions.includes(permission) || permissions.includes('system.admin');
    },

    // Check if user has any of the permissions
    hasAnyPermission(permissionList) {
        return permissionList.some(p => this.hasPermission(p));
    },

    // Login
    login(email, password) {
        const users = this.getUsers();
        const user = users.find(u => u.email.toLowerCase() === email.toLowerCase());
        
        if (!user) {
            this.logAudit('LOGIN_FAILED', null, { email, reason: 'User not found' });
            return { success: false, error: 'Invalid email or password' };
        }
        
        if (user.password !== password) { // In production: compare hashes!
            this.logAudit('LOGIN_FAILED', user.id, { reason: 'Wrong password' });
            return { success: false, error: 'Invalid email or password' };
        }
        
        if (user.status !== 'active') {
            this.logAudit('LOGIN_FAILED', user.id, { reason: 'Account inactive' });
            return { success: false, error: 'Account is not active. Contact admin.' };
        }

        // Create session (24 hours)
        const session = {
            userId: user.id,
            expiresAt: new Date(Date.now() + 24 * 60 * 60 * 1000).toISOString()
        };
        localStorage.setItem('auth_session', JSON.stringify(session));
        
        this.logAudit('LOGIN_SUCCESS', user.id, {});
        return { success: true, user };
    },

    // Logout
    logout() {
        const user = this.getCurrentUser();
        if (user) {
            this.logAudit('LOGOUT', user.id, {});
        }
        localStorage.removeItem('auth_session');
    },

    // Register new user (admin only in production)
    createUser(userData) {
        const users = this.getUsers();
        
        if (users.find(u => u.email.toLowerCase() === userData.email.toLowerCase())) {
            return { success: false, error: 'Email already exists' };
        }

        const newUser = {
            id: 'usr_' + Date.now(),
            email: userData.email,
            name: userData.name,
            password: userData.password, // In production: hash this!
            groups: userData.groups || [],
            status: 'active',
            createdAt: new Date().toISOString()
        };

        users.push(newUser);
        this.saveUsers(users);
        this.logAudit('USER_CREATED', this.getCurrentUser()?.id, { newUserId: newUser.id, email: newUser.email });
        
        return { success: true, user: newUser };
    },

    // Update user
    updateUser(userId, updates) {
        const users = this.getUsers();
        const index = users.findIndex(u => u.id === userId);
        
        if (index === -1) {
            return { success: false, error: 'User not found' };
        }

        users[index] = { ...users[index], ...updates };
        this.saveUsers(users);
        this.logAudit('USER_UPDATED', this.getCurrentUser()?.id, { targetUserId: userId, updates: Object.keys(updates) });
        
        return { success: true, user: users[index] };
    },

    // Delete user
    deleteUser(userId) {
        const users = this.getUsers();
        const filtered = users.filter(u => u.id !== userId);
        
        if (filtered.length === users.length) {
            return { success: false, error: 'User not found' };
        }

        this.saveUsers(filtered);
        this.logAudit('USER_DELETED', this.getCurrentUser()?.id, { deletedUserId: userId });
        
        return { success: true };
    },

    // Create group
    createGroup(groupData) {
        const groups = this.getGroups();
        
        const newGroup = {
            id: 'grp_' + Date.now(),
            name: groupData.name,
            description: groupData.description || '',
            permissions: groupData.permissions || [],
            color: groupData.color || '#6b7280'
        };

        groups.push(newGroup);
        this.saveGroups(groups);
        this.logAudit('GROUP_CREATED', this.getCurrentUser()?.id, { groupId: newGroup.id, name: newGroup.name });
        
        return { success: true, group: newGroup };
    },

    // Update group
    updateGroup(groupId, updates) {
        const groups = this.getGroups();
        const index = groups.findIndex(g => g.id === groupId);
        
        if (index === -1) {
            return { success: false, error: 'Group not found' };
        }

        groups[index] = { ...groups[index], ...updates };
        this.saveGroups(groups);
        this.logAudit('GROUP_UPDATED', this.getCurrentUser()?.id, { groupId, updates: Object.keys(updates) });
        
        return { success: true, group: groups[index] };
    },

    // Delete group
    deleteGroup(groupId) {
        if (groupId === 'grp_admin') {
            return { success: false, error: 'Cannot delete admin group' };
        }

        const groups = this.getGroups();
        const filtered = groups.filter(g => g.id !== groupId);
        this.saveGroups(filtered);

        // Remove group from all users
        const users = this.getUsers();
        users.forEach(u => {
            u.groups = u.groups.filter(g => g !== groupId);
        });
        this.saveUsers(users);
        
        this.logAudit('GROUP_DELETED', this.getCurrentUser()?.id, { deletedGroupId: groupId });
        
        return { success: true };
    },

    // Audit logging
    logAudit(action, actorId, details) {
        const logs = JSON.parse(localStorage.getItem('auth_audit') || '[]');
        logs.unshift({
            id: 'log_' + Date.now(),
            action,
            actorId,
            details,
            timestamp: new Date().toISOString(),
            ip: 'localhost' // In production: get real IP
        });
        // Keep last 500 logs
        localStorage.setItem('auth_audit', JSON.stringify(logs.slice(0, 500)));
    },

    // Get audit logs
    getAuditLogs(limit = 50) {
        const logs = JSON.parse(localStorage.getItem('auth_audit') || '[]');
        return logs.slice(0, limit);
    },

    // Page guard - redirect if no permission
    requirePermission(permissions, redirectUrl = 'login.html') {
        const user = this.getCurrentUser();
        
        if (!user) {
            window.location.href = redirectUrl;
            return false;
        }

        const permArray = Array.isArray(permissions) ? permissions : [permissions];
        if (!this.hasAnyPermission(permArray)) {
            window.location.href = 'access-denied.html';
            return false;
        }

        return true;
    },

    // Get all available permissions
    getAllPermissions() {
        return [
            { id: 'solar.view', name: 'View Solar', category: 'Solar', description: 'View solar monitoring data' },
            { id: 'solar.manage', name: 'Manage Solar', category: 'Solar', description: 'Control solar system settings' },
            { id: 'wind.view', name: 'View Wind', category: 'Wind', description: 'View wind monitoring data' },
            { id: 'wind.manage', name: 'Manage Wind', category: 'Wind', description: 'Control wind system settings' },
            { id: 'battery.view', name: 'View Battery', category: 'Battery', description: 'View battery monitoring data' },
            { id: 'battery.manage', name: 'Manage Battery', category: 'Battery', description: 'Control battery system settings' },
            { id: 'controlpanel.manage', name: 'Control Panel', category: 'System', description: 'Access device control panel' },
            { id: 'system.admin', name: 'Administrator', category: 'System', description: 'Full system administration' }
        ];
    }
};

// Initialize on load
AUTH.init();
